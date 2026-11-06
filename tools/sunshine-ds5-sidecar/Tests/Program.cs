using System.Diagnostics;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using Foundation.DualSense;
using Microsoft.Win32;
using Sunshine.Ds5Sidecar;

// A child exits immediately after a real registry field write, bypassing catch/finally.
if (args.Length == 4 && args[0] == "--crash")
{
    if (!args[1].StartsWith(@"Software\FoundationSunshineTests\GameInput-", StringComparison.Ordinal))
        throw new ArgumentException("Invalid isolated fixture");
    using var hive = Registry.CurrentUser.OpenSubKey(args[1], true)!;
    var step = int.Parse(args[3]);
    new GameInputRepair(hive, args[2], afterWrite: i => { if (i == step) Environment.Exit(73); }).Repair();
    throw new Exception("Crash point was not reached");
}

int count = 0;
void Require(bool valid, string label) { if (!valid) throw new Exception(label); ++count; }
void Refused(Action action, string label)
{
    bool rejected = false;
    try { action(); } catch (Exception e) when (e is IOException or InvalidDataException or JsonException or UnauthorizedAccessException) { rejected = true; }
    Require(rejected, label);
}

var root = @"Software\FoundationSunshineTests\GameInput-" + Guid.NewGuid().ToString("N");
var directory = Path.Combine(Path.GetTempPath(), "sunshine-gameinput-" + Guid.NewGuid().ToString("N"));
using var hiveRoot = Registry.CurrentUser.CreateSubKey(root);
var target = DualSenseGameInputMapping.Path;
RegistryTree? Tree() => RegistryTree.Read(hiveRoot, target);
void Reset(bool axes = false)
{
    hiveRoot.DeleteSubKeyTree(target, false);
    foreach (var node in DualSenseGameInputMapping.Template(axes, false).Nodes)
    {
        using var key = hiveRoot.CreateSubKey(target + (node.Key.Length == 0 ? "" : "\\" + node.Key));
        foreach (var value in node.Value) key.SetValue(value.Key, value.Value.Value(), value.Value.Kind);
    }
}
GameInputRepair Engine(string name, Action? guard = null, Action<int>? afterWrite = null) =>
    new(hiveRoot, Path.Combine(directory, name), guard, afterWrite);
ProcessStartInfo CrashProcess(string folder, int step)
{
    var host = Environment.ProcessPath ?? throw new IOException("Cannot locate the running test host");
    var start = new ProcessStartInfo(host) { UseShellExecute = false };
    // The build script runs the DLL through its pinned dotnet. Also permit the
    // ordinary apphost entry point without passing that executable its own DLL.
    if (!string.Equals(Path.GetFileNameWithoutExtension(host), Assembly.GetExecutingAssembly().GetName().Name, StringComparison.OrdinalIgnoreCase))
        start.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
    foreach (var arg in new[] { "--crash", root, folder, step.ToString() }) start.ArgumentList.Add(arg);
    return start;
}

try
{
    // In-memory ACLs exercise the elevated backup policy without changing any
    // real ProgramData directory or requiring administrator privileges.
    var adminSid = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
    var acl = new DirectorySecurity();
    acl.SetOwner(adminSid);
    acl.AddAccessRule(new FileSystemAccessRule(adminSid, FileSystemRights.FullControl, AccessControlType.Allow));
    GameInputCommand.ValidateBackupSecurity(acl);
    ++count;
    acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.WorldSid, null), FileSystemRights.Write, AccessControlType.Allow));
    Refused(() => GameInputCommand.ValidateBackupSecurity(acl), "world-writable backup accepted");
    var noDacl = new DirectorySecurity();
    var raw = new RawSecurityDescriptor(ControlFlags.DiscretionaryAclPresent, adminSid, adminSid, null, null);
    var aclBytes = new byte[raw.BinaryLength]; raw.GetBinaryForm(aclBytes, 0);
    noDacl.SetSecurityDescriptorBinaryForm(aclBytes);
    Refused(() => GameInputCommand.ValidateBackupSecurity(noDacl), "null DACL backup accepted");
    Require(Engine("absent").Check().State == "mapping_missing" && !Directory.Exists(directory), "check must be read-only");
    Require(Engine("absent").Repair().State == "mapping_missing" && Tree() == null, "missing table must not be manufactured");
    foreach (var axes in new[] { false, true })
    {
        Reset(axes);
        var before = Tree()!;
        var engine = Engine("repair-" + axes);
        var result = engine.Repair();
        Require(result.State == "repaired" && result.Changes.Count == (axes ? 7 : 10), "wrong repair changes");
        Require(Tree()!.SameAs(DualSenseGameInputMapping.Template(true, true)), "full mapping differs from target");
        Require(engine.Repair().State == "corrected_mapping", "repair is not idempotent");
        Require(Directory.GetFiles(Path.Combine(directory, "repair-" + axes), "*.json").Length == 1, "no-op repair wrote another backup");
        var backup = JsonSerializer.Deserialize<GameInputJournal>(File.ReadAllText(Path.Combine(directory, "repair-" + axes, result.BackupId + ".json")))!;
        Require(backup.Before.SameAs(before) && backup.BeforeHash == before.Hash(), "backup did not capture the full original tree");
        Require(engine.Restore(result.BackupId!).State == "restored" && Tree()!.SameAs(before), "restore did not recover exact original");
        Require(engine.Restore(result.BackupId!).State == "already_restored", "restore is not idempotent");
    }
    foreach (var variant in new[] { "extra", "default", "type", "partial", "combined", "missing" })
    {
        Reset();
        using (var key = hiveRoot.OpenSubKey(target + @"\Gamepad\RightTrigger", true)!)
        {
            if (variant == "extra") key.SetValue("Other", new byte[] { 1, 2, 3 });
            if (variant == "default") key.SetValue("", "user-owned");
            if (variant == "type") key.SetValue("AxisIndex", "5", RegistryValueKind.String);
            if (variant == "partial") key.SetValue("AxisIndex", 4);
            if (variant == "combined") key.SetValue("AxisIndex", 6);
            if (variant == "missing") key.DeleteValue("AxisIndex");
        }
        var before = Tree()!;
        Require(Engine(variant).Repair().State == "custom_mapping" && Tree()!.SameAs(before), "custom mapping modified: " + variant);
        Require(!Directory.Exists(Path.Combine(directory, variant)), "custom mapping created a backup");
    }
    Reset(true);
    foreach (var c in DualSenseGameInputMapping.Changes.Skip(3).Take(4))
    {
        using var key = hiveRoot.OpenSubKey(target + "\\Gamepad\\" + c.Key, true)!;
        key.SetValue(c.Value, c.Correct);
    }
    Require(Engine("navigation").Check().State == "navigation_fixed", "seven-field intermediate mapping not recognized");
    Require(Engine("navigation").Repair().Changes.Count == 3, "face-only repair changed other fields");
    Reset();
    var baseline = Tree()!;
    Refused(() => Engine("held", () => throw new IOException("live holder")).Repair(), "held mapping accepted");
    Require(Tree()!.SameAs(baseline), "holder guard changed mapping");
    Directory.CreateDirectory(directory);
    File.WriteAllText(Path.Combine(directory, "not-directory"), "fixture");
    Require(new GameInputRepair(hiveRoot, Path.Combine(directory, "not-directory")).Repair().State == "inspection_failed", "invalid backup storage accepted");
    Require(Tree()!.SameAs(baseline), "backup failure changed mapping");

    for (int step = 0; step < 10; ++step)
    {
        Reset();
        var engine = Engine("failure-" + step, afterWrite: i => { if (i == step) throw new IOException("injected failure"); });
        Require(engine.Repair().State == "repair_failed" && Tree()!.SameAs(baseline), "failure rollback at step " + step);
        Require(engine.Check().State == "legacy_mapping", "rolled-back operation is still pending");
    }
    for (int step = 0; step < 10; ++step)
    {
        Reset();
        var backupDir = Path.Combine(directory, "crash-" + step);
        var start = CrashProcess(backupDir, step);
        using var child = Process.Start(start)!;
        Require(child.WaitForExit(15_000) && child.ExitCode == 73, "crash child did not reach step " + step);
        var engine = new GameInputRepair(hiveRoot, backupDir);
        var pending = engine.Check();
        Require(pending.State == "recovery_required", "crash not detected at step " + step);
        Require(engine.Repair().State == "recovery_required", "pending migration was overwritten");
        Require(engine.Restore(pending.BackupId!).State == "restored" && Tree()!.SameAs(baseline), "crash recovery failed at step " + step);
    }
    Reset();
    var ordered = Engine("ordered-recovery");
    var older = ordered.Repair(); ordered.Restore(older.BackupId!);
    var crashStart = CrashProcess(Path.Combine(directory, "ordered-recovery"), 0);
    using (var child = Process.Start(crashStart)!)
        Require(child.WaitForExit(15_000) && child.ExitCode == 73, "ordered recovery fixture did not crash");
    var interrupted = Tree()!;
    Refused(() => ordered.Restore(older.BackupId!), "an older backup bypassed pending recovery");
    Require(Tree()!.SameAs(interrupted), "older restore changed a pending migration");
    var newest = ordered.Check().BackupId!;
    Require(ordered.Restore(newest).State == "restored" && Tree()!.SameAs(baseline), "newest operation could not be recovered");
    Reset();
    var conflict = Engine("conflict");
    var fixedResult = conflict.Repair();
    using (var key = hiveRoot.OpenSubKey(target + @"\Gamepad\RightTrigger", true)!) key.SetValue("AxisIndex", 42);
    var changed = Tree()!;
    Refused(() => conflict.Restore(fixedResult.BackupId!), "foreign field overwritten by restore");
    Require(Tree()!.SameAs(changed), "restore conflict changed configuration");
    Reset();
    var absent = Engine("restore-absent"); var fixedAbsent = absent.Repair();
    hiveRoot.DeleteSubKeyTree(target);
    Require(absent.Restore(fixedAbsent.BackupId!).State == "restored" && Tree()!.SameAs(baseline), "missing tree not restored from valid backup");
    // Simulate a process exit during reconstruction of a wholly missing tree.
    var partialRecord = Path.Combine(directory, "restore-absent", fixedAbsent.BackupId + ".json");
    var partial = JsonSerializer.Deserialize<GameInputJournal>(File.ReadAllText(partialRecord))!;
    partial.Status = "restoring"; partial.RecreatingMissing = true;
    File.WriteAllText(partialRecord, JsonSerializer.Serialize(partial));
    hiveRoot.DeleteSubKeyTree(target);
    using (var key = hiveRoot.CreateSubKey(target + @"\Gamepad\A")) key.SetValue("ButtonIndex", 0);
    Require(absent.Check().State == "recovery_required", "partial missing-tree restore was not detected");
    Require(absent.Restore(fixedAbsent.BackupId!).State == "restored" && Tree()!.SameAs(baseline), "partial missing-tree restore did not resume");
    Refused(() => absent.Restore("../outside"), "backup path traversal accepted");
    var recordPath = Path.Combine(directory, "restore-absent", fixedAbsent.BackupId + ".json");
    var text = File.ReadAllText(recordPath);
    File.WriteAllText(recordPath, text.Replace("054C0CE600010005", "054C0DF200010005"));
    Refused(() => absent.Restore(fixedAbsent.BackupId!), "wrong identity backup accepted");
    Require(Tree()!.SameAs(baseline), "invalid backup changed mapping");
    // A concurrent edit after backup but before any write must not leave a
    // pending record that permanently blocks this now-custom mapping.
    Reset();
    var prewriteDirectory = Path.Combine(directory, "prewrite");
    var beforeWrite = new GameInputRepair(hiveRoot, prewriteDirectory, secureFile: _ => {
        using var key = hiveRoot.OpenSubKey(target + @"\Gamepad\A", true)!;
        key.SetValue("ButtonIndex", 99);
    });
    var notStarted = beforeWrite.Repair();
    Require(notStarted.State == "repair_failed" && !notStarted.RestartRequired, "unstarted repair reported success or wrote fields");
    Require(beforeWrite.Check().State == "custom_mapping" && Directory.GetFiles(prewriteDirectory, "*.json").Length == 0,
        "unstarted intent blocks future inspection");
    Require((int)Tree()!.Get(@"Gamepad\A", "ButtonIndex").Value() == 99, "unstarted repair overwrote the concurrent edit");
    // Concurrent edits must survive both the failed apply and its attempted rollback.
    Reset();
    var concurrent = Engine("concurrent", afterWrite: i => {
        if (i != 0) return;
        using var key = hiveRoot.OpenSubKey(target + @"\Gamepad\A", true)!;
        key.SetValue("ButtonIndex", 99);
    });
    Require(concurrent.Repair().State == "recovery_required", "concurrent edit did not produce conflict");
    Require((int)Tree()!.Get(@"Gamepad\A", "ButtonIndex").Value() == 99, "concurrent edit overwritten");
    Console.WriteLine($"PASS: {count} GameInput checks; exact mappings, no-op/custom preservation, backup/restore, conflicts and ten crash points");
}
finally
{
    hiveRoot.Dispose();
    Registry.CurrentUser.DeleteSubKeyTree(root, false);
    if (Directory.Exists(directory)) Directory.Delete(directory, true);
}
