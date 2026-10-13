using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32;

if (args.Length < 2) throw new ArgumentException("extract <assembly> <directory> | verify <original> <patched> | test <patched> <profile>");
var context = new AssemblyLoadContext("payload", isCollectible: true);
var assembly = context.LoadFromAssemblyPath(Path.GetFullPath(args[1]));
if (args[0] == "extract")
{
    Directory.CreateDirectory(args[2]);
    var resources = new System.Text.StringBuilder("<Project><ItemGroup>");
    int index = 0;
    foreach (var name in assembly.GetManifestResourceNames())
    {
        var file = Path.GetFullPath(Path.Combine(args[2], $"{index++:D4}.bin"));
        using var input = assembly.GetManifestResourceStream(name)!;
        using var output = File.Create(file);
        input.CopyTo(output);
        resources.Append($"<EmbeddedResource Include=\"{System.Security.SecurityElement.Escape(file)}\"><LogicalName>{System.Security.SecurityElement.Escape(name)}</LogicalName></EmbeddedResource>");
    }
    resources.Append("</ItemGroup></Project>");
    File.WriteAllText(Path.Combine(args[2], "resources.props"), resources.ToString());
    Console.WriteLine($"Extracted {assembly.GetManifestResourceNames().Length} upstream resources");
}
else if (args[0] == "verify")
{
    var patchedContext = new AssemblyLoadContext("patched", isCollectible: true);
    var patched = patchedContext.LoadFromAssemblyPath(Path.GetFullPath(args[2]));
    Require(assembly.GetName().FullName == patched.GetName().FullName, "assembly identity changed");
    Require(Api(assembly).SequenceEqual(Api(patched)), "public SDK API changed");
    var names = assembly.GetManifestResourceNames().Order().ToArray();
    Require(names.SequenceEqual(patched.GetManifestResourceNames().Order()), "resource catalog changed");
    foreach (var name in names)
    {
        using var originalStream = assembly.GetManifestResourceStream(name)!;
        using var patchedStream = patched.GetManifestResourceStream(name)!;
        Require(SHA256.HashData(originalStream).SequenceEqual(SHA256.HashData(patchedStream)), $"resource changed: {name}");
    }
    Require(patched.GetCustomAttributes<AssemblyMetadataAttribute>().Any(a =>
        a.Key == "FoundationCorePatch" && a.Value == "registry-isolation-1"), "patch marker missing");
    Console.WriteLine($"SDK identity/API unchanged; {names.Length} resources byte-identical");
}
else if (args[0] == "test")
{
    // Only the two registry writers run, with HKLM redirected inside this
    // process to a disposable HKCU hive. No PnP, driver or cleanup is invoked.
    var isolatedPath = $@"Software\FoundationSunshineTests\CorePatch-{Guid.NewGuid():N}";
    using var isolated = Registry.CurrentUser.CreateSubKey(isolatedPath);
    var gameInputPath = @"SYSTEM\CurrentControlSet\Control\GameInput\Devices\054C0CE600010005";
    var oemPath = @"SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_054C&PID_0CE6";
    using (var mapping = isolated.CreateSubKey(gameInputPath)) mapping.SetValue("CustomMapping", new byte[] { 1, 2, 3 });
    using (var oem = isolated.CreateSubKey(oemPath))
    {
        oem.SetValue("OEMName", "Physical controller");
        oem.SetValue("OEMData", new byte[] { 4, 5, 6 });
        using var axis = oem.CreateSubKey("Axes\\0");
        axis.SetValue("Calibration", "user-owned");
    }
    var orchestrator = assembly.GetType("HIDMaestro.Internal.DeviceOrchestrator", throwOnError: true)!;
    var profileType = assembly.GetType("HIDMaestro.Internal.ControllerProfile", throwOnError: true)!;
    var profileDocument = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(args[2]))!.AsObject();
    // Layout's polymorphic converter belongs to ProfileDatabase. These writer
    // tests need the real descriptor and identity, not the UI layout.
    profileDocument.Remove("layout");
    var profileJson = profileDocument.ToJsonString();
    var writeGameInput = orchestrator.GetMethod("WriteGameInputRegistry", BindingFlags.NonPublic | BindingFlags.Static)!;
    var writeInstance = orchestrator.GetMethod("WriteInstanceConfig", BindingFlags.NonPublic | BindingFlags.Static)!;
    var redirected = false;
    try
    {
        Require(Native.RegOverridePredefKey(unchecked((nint)(int)0x80000002), isolated.Handle.DangerousGetHandle()) == 0, "HKLM redirect failed");
        redirected = true;
        foreach (var descriptorUsage in new[] { "05010904", "05010905" })
        {
            var json = profileJson.Replace("05010904", descriptorUsage);
            var profile = JsonSerializer.Deserialize(json, profileType)!;
            writeGameInput.Invoke(null, new[] { profile });
            writeInstance.Invoke(null, new object[] { 0, profile });
            using var mapping = isolated.OpenSubKey(gameInputPath)!;
            Require(mapping.GetSubKeyNames().Length == 0 && mapping.GetValueNames().SequenceEqual(new[] { "CustomMapping" }), "GameInput tree changed");
            Require(((byte[])mapping.GetValue("CustomMapping")!).SequenceEqual(new byte[] { 1, 2, 3 }), "GameInput value changed");
            using var oem = isolated.OpenSubKey(oemPath)!;
            Require((string)oem.GetValue("OEMName")! == "Physical controller", "OEM name changed");
            Require(((byte[])oem.GetValue("OEMData")!).SequenceEqual(new byte[] { 4, 5, 6 }), "OEM data deleted");
            using var axis = oem.OpenSubKey("Axes\\0")!;
            Require((string)axis.GetValue("Calibration")! == "user-owned", "axes deleted");
            using var instance = isolated.OpenSubKey(@"SOFTWARE\HIDMaestro\Controller0")!;
            Require(((byte[])instance.GetValue("ReportDescriptor")!).Length > 0, "instance descriptor missing");
            Require((int)instance.GetValue("VendorId")! == 0x054c, "instance VID missing");
        }
        var parse = orchestrator.GetMethod("InterfaceInstanceId", BindingFlags.NonPublic | BindingFlags.Static)!;
        const string guid = "{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
        Require((string?)parse.Invoke(null, new[] { $"##?#ROOT#HIDCLASS#0000#{guid}" }) == @"ROOT\HIDCLASS\0000", "interface parser failed");
        foreach (var invalid in new[] { "foreignROOT#device", $"##?#ROOT#HIDCLASS##{guid}", "##?#ROOT#HIDCLASS#0000#invalid" })
            Require(parse.Invoke(null, new[] { invalid }) == null, "malformed interface accepted");
        var ownsInterface = orchestrator.GetMethod("IsOwnedInterfaceKey", BindingFlags.NonPublic | BindingFlags.Static)!;
        using (var foreign = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\ROOT\HIDCLASS\0000"))
            foreign.SetValue("HardwareID", new[] { @"root\vJoy" }, RegistryValueKind.MultiString);
        Require(!(bool)ownsInterface.Invoke(null, new[] { $"##?#ROOT#HIDCLASS#0000#{guid}" })!, "foreign ROOT interface claimed");
        Require(!(bool)ownsInterface.Invoke(null, new[] { $"##?#SWD#VID_054C&PID_0CE6#missing#{guid}" })!, "unknown SWD interface claimed");
        using (var owned = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\ROOT\HIDCLASS\0001"))
            owned.SetValue("HardwareID", new[] { @"root\HIDMaestroGamepad" }, RegistryValueKind.MultiString);
        Require((bool)ownsInterface.Invoke(null, new[] { $"##?#ROOT#HIDCLASS#0001#{guid}" })!, "owned shared interface missed");
        Require((bool)ownsInterface.Invoke(null, new[] { $"##?#swd#HIDMAESTRO#0000#{guid}" })!, "reserved SDK interface missed");
        using (var child = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\HID\HIDCLASS\unknown"))
            child.SetValue("HardwareID", new[] { "HID_DEVICE_SYSTEM_GAME" }, RegistryValueKind.MultiString);
        using (var child = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\HID\HIDCLASS\owned"))
            child.SetValue("HardwareID", new[] { @"root\HIDMaestro" }, RegistryValueKind.MultiString);
        var collect = orchestrator.GetMethod("CollectOwnedHidChildren", BindingFlags.NonPublic | BindingFlags.Static)!;
        var ownedChildren = (HashSet<string>)collect.Invoke(null, null)!;
        Require(!ownedChildren.Contains(@"HID\HIDCLASS\unknown"), "unknown-parent HID child claimed");
        Require(ownedChildren.Contains(@"HID\HIDCLASS\owned"), "owned HID child missed");
        // The compiled orchestrator must contain no shared OEM/slot/GameInput
        // path, covering the launch and full-uninstall sweep without touching PnP.
        var sharedPrefixes = new[] { "\\GameInput\\Devices\\", "\\MediaProperties\\PrivateProperties\\Joystick", "\\MediaResources\\Joystick" };
        // Other SDK APIs legitimately contain OEM strings. Check only the
        // orchestrator's ldstr tokens rather than the whole assembly.
        foreach (var method in orchestrator.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
        {
            var il = method.GetMethodBody()?.GetILAsByteArray();
            if (il == null) continue;
            foreach (var literal in IlStrings(method, il))
                Require(!sharedPrefixes.Any(p => literal.Contains(p, StringComparison.OrdinalIgnoreCase)), $"shared path remains in {method.Name}");
        }
        Console.WriteLine("PASS: Joystick/Game Pad preserve shared mapping, OEM, axes; instance config works; sweep contains no shared setting paths");
    }
    finally
    {
        if (redirected) Require(Native.RegOverridePredefKey(unchecked((nint)(int)0x80000002), 0) == 0, "HKLM restore failed");
        isolated.Dispose();
        Registry.CurrentUser.DeleteSubKeyTree(isolatedPath);
    }
}
else throw new ArgumentException("Unknown command");

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static string[] Api(Assembly a) => a.GetExportedTypes().SelectMany(t =>
    t.GetMembers(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
        .Select(m => $"{t.FullName}: {m.MemberType} {m}"))
    .Concat(a.GetExportedTypes().Select(t => $"type {t.FullName}: {t.BaseType?.FullName}"))
    .Order().ToArray();

static IEnumerable<string> IlStrings(MethodInfo method, byte[] il)
{
    var opcodes = typeof(System.Reflection.Emit.OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static)
        .Select(f => (System.Reflection.Emit.OpCode)f.GetValue(null)!)
        .ToDictionary(o => unchecked((ushort)o.Value));
    for (int i = 0; i < il.Length;)
    {
        ushort value = il[i++];
        if (value == 0xfe) value = (ushort)(0xfe00 | il[i++]);
        var operand = opcodes[value].OperandType;
        if (operand == System.Reflection.Emit.OperandType.InlineString)
            yield return method.Module.ResolveString(BitConverter.ToInt32(il, i));
        i += operand switch
        {
            System.Reflection.Emit.OperandType.InlineNone => 0,
            System.Reflection.Emit.OperandType.ShortInlineBrTarget or
            System.Reflection.Emit.OperandType.ShortInlineI or
            System.Reflection.Emit.OperandType.ShortInlineVar => 1,
            System.Reflection.Emit.OperandType.InlineVar => 2,
            System.Reflection.Emit.OperandType.InlineI8 or System.Reflection.Emit.OperandType.InlineR => 8,
            System.Reflection.Emit.OperandType.InlineSwitch => 4 + 4 * BitConverter.ToInt32(il, i),
            _ => 4,
        };
    }
}

static class Native
{
    [DllImport("advapi32.dll")]
    internal static extern int RegOverridePredefKey(nint key, nint replacement);
}
