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
    using var compatibility = JsonDocument.Parse(File.ReadAllText(args[3]));
    var usbip = compatibility.RootElement.GetProperty("usbip");
    var originalInstaller = assembly.GetType("HIDMaestro.Internal.Usbip.UsbipDriverInstaller", true)!;
    Require((string)originalInstaller.GetField("Version")!.GetRawConstantValue()! == usbip.GetProperty("version").GetString(),
        "contract and upstream USB/IP ABI version disagree");
    var pinnedDriverHash = (string)originalInstaller.GetMethod("PinnedUdeHash", BindingFlags.NonPublic | BindingFlags.Static)!
        .Invoke(null, new object[] { "usbip2_ude.sys" })!;
    Require(pinnedDriverHash == usbip.GetProperty("driver_sha256").GetString(), "contract and upstream driver hash disagree");
    Require((string)originalInstaller.GetProperty("InstallerSha256", BindingFlags.NonPublic | BindingFlags.Static)!.GetValue(null)! ==
        usbip.GetProperty("installer_sha256").GetString(), "contract and upstream installer hash disagree");
    var names = assembly.GetManifestResourceNames().Order().ToArray();
    Require(names.Append("Foundation.ComponentCompatibility.json").Order().SequenceEqual(patched.GetManifestResourceNames().Order()), "resource catalog changed");
    using (var contract = patched.GetManifestResourceStream("Foundation.ComponentCompatibility.json")!)
        Require(SHA256.HashData(contract).SequenceEqual(SHA256.HashData(File.ReadAllBytes(args[3]))), "compatibility contract changed");
    foreach (var name in names)
    {
        using var originalStream = assembly.GetManifestResourceStream(name)!;
        using var patchedStream = patched.GetManifestResourceStream(name)!;
        Require(SHA256.HashData(originalStream).SequenceEqual(SHA256.HashData(patchedStream)), $"resource changed: {name}");
    }
    Require(patched.GetCustomAttributes<AssemblyMetadataAttribute>().Any(a =>
        a.Key == "FoundationCorePatch" && a.Value == "device-ownership-1"), "patch marker missing");
    Require(patched.GetCustomAttributes<AssemblyMetadataAttribute>().Any(a =>
        a.Key == "FoundationUsbipPolicy" && a.Value == "externally-managed-v1"), "external transport policy missing");
    Console.WriteLine($"SDK identity/API unchanged; {names.Length} resources byte-identical");
}
else if (args[0] == "test")
{
    var transport = assembly.GetType("HIDMaestro.Internal.Usbip.SharedUsbipTransport", true)!;
    var validate = transport.GetMethod("ValidateState", BindingFlags.NonPublic | BindingFlags.Static)!;
    using var contract = JsonDocument.Parse(File.ReadAllText(args[3]));
    var driverHash = contract.RootElement.GetProperty("usbip").GetProperty("driver_sha256").GetString()!;
    validate.Invoke(null, new object?[] { true, driverHash });
    foreach (var state in new[] { (false, driverHash, "USBIP-SETUP-001:"), (true, new string('0', 64), "USBIP-EXEC-006:") })
    {
        try { validate.Invoke(null, new object?[] { state.Item1, state.Item2 }); Require(false, "incompatible driver accepted"); }
        catch (TargetInvocationException error) when (error.InnerException is InvalidOperationException invalid)
        { Require(invalid.Message.StartsWith(state.Item3), "incorrect shared transport diagnosis"); }
    }
    // The original public entry point must consist solely of the read-only
    // guard. It cannot reach installer, restart, replacement or owner stamping.
    var installer = assembly.GetType("HIDMaestro.Internal.Usbip.UsbipDriverInstaller", true)!;
    var ensure = installer.GetMethod("EnsureInstalled", BindingFlags.Public | BindingFlags.Static)!;
    var il = ensure.GetMethodBody()!.GetILAsByteArray()!;
    Require(il.Length == 6 && il[0] == 0x28 && il[^1] == 0x2a &&
        ensure.Module.ResolveMethod(BitConverter.ToInt32(il, 1))!.DeclaringType == transport,
        "runtime deployment entry point escaped the read-only transport guard");
    Console.WriteLine("Shared transport: missing/mismatched drivers fail closed; deployment entry point is read-only");
    // Run registry-only SDK paths with HKLM redirected inside this process
    // to a disposable HKCU hive. No driver deployment or PnP removal runs.
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
    Require(profileDocument["descriptor"]!.GetValue<string>().StartsWith("05010905", StringComparison.Ordinal),
        "native DualSense Game Pad descriptor missing");
    // Layout's polymorphic converter belongs to ProfileDatabase. These writer
    // tests need the real descriptor and identity, not the UI layout.
    profileDocument.Remove("layout");
    var profileJson = profileDocument.ToJsonString();
    var writeGameInput = orchestrator.GetMethod("ApplyGameInputMapping", BindingFlags.NonPublic | BindingFlags.Static)!;
    var ledger = assembly.GetType("HIDMaestro.Internal.RegistryLedger", throwOnError: true)!;
    var release = ledger.GetMethod("Release", BindingFlags.NonPublic | BindingFlags.Static)!;
    var gameInput = assembly.GetType("HIDMaestro.Internal.GameInputMapping", throwOnError: true)!;
    var writeLegacyTable = gameInput.GetMethod("WriteTable", BindingFlags.NonPublic | BindingFlags.Static)!;
    var writeInstance = orchestrator.GetMethod("WriteInstanceConfig", BindingFlags.NonPublic | BindingFlags.Static)!;
    var redirected = false;
    try
    {
        Require(Native.RegOverridePredefKey(unchecked((nint)(int)0x80000002), isolated.Handle.DangerousGetHandle()) == 0, "HKLM redirect failed");
        redirected = true;
        foreach (var descriptorUsage in new[] { "05010904", "05010905" })
        {
            var json = profileJson.Replace("05010905", descriptorUsage);
            var profile = JsonSerializer.Deserialize(json, profileType)!;
            writeGameInput.Invoke(null, new object[] { 0, profile });
            writeInstance.Invoke(null, new object[] { 0, profile });
            using var mapping = isolated.OpenSubKey(gameInputPath)!;
            Require(mapping.GetSubKeyNames().Length == 0 && mapping.GetValueNames().SequenceEqual(new[] { "CustomMapping" }), "GameInput tree changed");
            Require(((byte[])mapping.GetValue("CustomMapping")!).SequenceEqual(new byte[] { 1, 2, 3 }), "GameInput value changed");
            using var oem = isolated.OpenSubKey(oemPath)!;
            Require((string)oem.GetValue("OEMName")! == "DualSense Wireless Controller", "OEM name was not claimed");
            release.Invoke(null, new object[] { 0 });
            Require((string)oem.GetValue("OEMName")! == "Physical controller", "OEM name was not restored");
            Require(((byte[])oem.GetValue("OEMData")!).SequenceEqual(new byte[] { 4, 5, 6 }), "OEM data deleted");
            using var axis = oem.OpenSubKey("Axes\\0")!;
            Require((string)axis.GetValue("Calibration")! == "user-owned", "axes deleted");
            using var instance = isolated.OpenSubKey(@"SOFTWARE\HIDMaestro\Controller0")!;
            Require(((byte[])instance.GetValue("ReportDescriptor")!).Length > 0, "instance descriptor missing");
            Require((int)instance.GetValue("VendorId")! == 0x054c, "instance VID missing");
        }
        // Neither Joystick nor Game Pad JSON profiles get a new table.
        // Exact legacy bytes are removed; any foreign edit protects the whole key.
        isolated.DeleteSubKeyTree(gameInputPath);
        foreach (var descriptorUsage in new[] { "05010904", "05010905" })
        {
            var profile = JsonSerializer.Deserialize(profileJson.Replace("05010905", descriptorUsage), profileType)!;
            writeGameInput.Invoke(null, new object[] { 0, profile });
            using (var absent = isolated.OpenSubKey(gameInputPath))
                Require(absent == null, "JSON profile acquired a GameInput table");
            writeLegacyTable.Invoke(null, new object[] { (ushort)0x054c, (ushort)0x0ce6 });
            using (var legacy = isolated.OpenSubKey(gameInputPath)) Require(legacy != null, "legacy fixture missing");
            writeGameInput.Invoke(null, new object[] { 0, profile });
            using (var absent = isolated.OpenSubKey(gameInputPath))
                Require(absent == null, "exact legacy GameInput table survived");
        }
        writeLegacyTable.Invoke(null, new object[] { (ushort)0x054c, (ushort)0x0ce6 });
        using (var foreign = isolated.CreateSubKey(gameInputPath)) foreign.SetValue("ForeignEdit", 1);
        var jsonProfile = JsonSerializer.Deserialize(profileJson, profileType)!;
        writeGameInput.Invoke(null, new object[] { 0, jsonProfile });
        using (var foreign = isolated.OpenSubKey(gameInputPath))
            Require(foreign?.GetValue("ForeignEdit") is int edit && edit == 1, "edited legacy table removed");
        writeInstance.Invoke(null, new object[] { 0, jsonProfile });
        using (var oem = isolated.CreateSubKey(oemPath)) oem.SetValue("OEMName", "Changed by another app");
        release.Invoke(null, new object[] { 0 });
        using (var oem = isolated.OpenSubKey(oemPath))
            Require((string)oem!.GetValue("OEMName")! == "Changed by another app", "external OEM name overwritten on release");
        var parse = orchestrator.GetMethod("InterfaceInstanceId", BindingFlags.NonPublic | BindingFlags.Static)!;
        const string guid = "{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
        Require((string?)parse.Invoke(null, new[] { $"##?#ROOT#HIDCLASS#0000#{guid}" }) == @"ROOT\HIDCLASS\0000", "interface parser failed");
        foreach (var invalid in new[] { "foreignROOT#device", $"##?#ROOT#HIDCLASS##{guid}", "##?#ROOT#HIDCLASS#0000#invalid" })
            Require(parse.Invoke(null, new[] { invalid }) == null, "malformed interface accepted");
        var ownsInterface = orchestrator.GetMethod("IsOwnedInterfaceKey", BindingFlags.NonPublic | BindingFlags.Static)!;
        using (var foreign = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\ROOT\HIDCLASS\0000"))
            foreign.SetValue("HardwareID", new[] { @"root\vJoy" }, RegistryValueKind.MultiString);
        Require(!(bool)ownsInterface.Invoke(null, new object?[] { $"##?#ROOT#HIDCLASS#0000#{guid}", null })!, "foreign ROOT interface claimed");
        Require(!(bool)ownsInterface.Invoke(null, new object?[] { $"##?#SWD#VID_054C&PID_0CE6#missing#{guid}", null })!, "unknown SWD interface claimed");
        using (var owned = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\ROOT\HIDCLASS\0001"))
            owned.SetValue("HardwareID", new[] { @"root\HIDMaestroGamepad" }, RegistryValueKind.MultiString);
        Require((bool)ownsInterface.Invoke(null, new object?[] { $"##?#ROOT#HIDCLASS#0001#{guid}", null })!, "owned shared interface missed");
        Require((bool)ownsInterface.Invoke(null, new object?[] { $"##?#swd#HIDMAESTRO#0000#{guid}", null })!, "reserved SDK interface missed");
        using (var child = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\HID\HIDCLASS\unknown"))
            child.SetValue("HardwareID", new[] { "HID_DEVICE_SYSTEM_GAME" }, RegistryValueKind.MultiString);
        using (var child = isolated.CreateSubKey(@"SYSTEM\CurrentControlSet\Enum\HID\HIDCLASS\owned"))
            child.SetValue("HardwareID", new[] { @"root\HIDMaestro" }, RegistryValueKind.MultiString);
        const string parentPath = @"SYSTEM\CurrentControlSet\Enum\ROOT\HIDMAESTRO\foundation-test-parent";
        using (var parent = isolated.CreateSubKey(parentPath))
        {
            parent.SetValue("HardwareID", new[] { @"root\HIDMaestroGamepad" }, RegistryValueKind.MultiString);
            parent.SetValue("ParentIdPrefix", "foundation-test-prefix");
        }
        const string inheritedChild = @"HID\HIDCLASS\foundation-test-prefix&0000";
        using (var child = isolated.CreateSubKey($@"SYSTEM\CurrentControlSet\Enum\{inheritedChild}"))
            child.SetValue("HardwareID", new[] { "HID_DEVICE_SYSTEM_GAME" }, RegistryValueKind.MultiString);
        var collect = orchestrator.GetMethod("CollectOwnedHidChildren", BindingFlags.NonPublic | BindingFlags.Static)!;
        var ownedChildren = (HashSet<string>)collect.Invoke(null, null)!;
        Require(!ownedChildren.Contains(@"HID\HIDCLASS\unknown"), "unknown-parent HID child claimed");
        Require(ownedChildren.Contains(@"HID\HIDCLASS\owned"), "owned HID child missed");
        Require(ownedChildren.Contains(inheritedChild), "owned parent's HID child missed");
        isolated.DeleteSubKeyTree(parentPath);
        var afterParentRemoval = (HashSet<string>)collect.Invoke(null, null)!;
        Require(!afterParentRemoval.Contains(inheritedChild) && ownedChildren.Contains(inheritedChild),
            "pre-removal ownership evidence was not preserved separately from the missing parent");
        // Exercise the actual registry-only interface sweep, not just its predicate.
        var classPath = $@"SYSTEM\CurrentControlSet\Control\DeviceClasses\{guid}";
        var foreignInterface = $"##?#ROOT#HIDCLASS#0000#{guid}";
        var missingInterface = $"##?#ROOT#HIDCLASS#missing#{guid}";
        var ownedInterface = $"##?#ROOT#HIDCLASS#0001#{guid}";
        using (var classes = isolated.CreateSubKey(classPath))
            foreach (var name in new[] { foreignInterface, missingInterface, ownedInterface })
                classes.CreateSubKey(name).Dispose();
        var collectInterfaces = orchestrator.GetMethod("CollectOwnedInterfaceInstances", BindingFlags.NonPublic | BindingFlags.Static)!;
        var ownedInterfaces = (HashSet<string>)collectInterfaces.Invoke(null, null)!;
        Require(ownedInterfaces.Contains(@"ROOT\HIDCLASS\0001"), "owned interface evidence was not captured");
        Require(!ownedInterfaces.Contains(@"ROOT\HIDCLASS\0000") && !ownedInterfaces.Contains(@"ROOT\HIDCLASS\missing"),
            "foreign or unknown interface entered ownership snapshot");
        // Match production ordering: remove the shared-enumerator parent first.
        isolated.DeleteSubKeyTree(@"SYSTEM\CurrentControlSet\Enum\ROOT\HIDCLASS\0001");
        Require(!(bool)ownsInterface.Invoke(null, new object?[] { ownedInterface, null })!,
            "missing parent unexpectedly proved ownership without evidence");
        Require((bool)ownsInterface.Invoke(null, new object?[] { ownedInterface, ownedInterfaces })!,
            "pre-removal interface evidence was lost");
        orchestrator.GetMethod("RemoveOwnedInterfaceEntries", BindingFlags.NonPublic | BindingFlags.Static)!
            .Invoke(null, new object[] { ownedInterfaces });
        using (var classes = isolated.OpenSubKey(classPath)!)
        {
            using var foreign = classes.OpenSubKey(foreignInterface);
            using var missing = classes.OpenSubKey(missingInterface);
            using var owned = classes.OpenSubKey(ownedInterface);
            Require(foreign != null && missing != null, "foreign or unowned interface removed");
            Require(owned == null, "owned interface survived cleanup");
        }
        Console.WriteLine("PASS: JSON profiles acquire no mapping; exact legacy tables removed; foreign mappings/OEM/axes preserved; OEM claims restore safely; interface cleanup and HID ownership guards pass");
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

static class Native
{
    [DllImport("advapi32.dll")]
    internal static extern int RegOverridePredefKey(nint key, nint replacement);
}
