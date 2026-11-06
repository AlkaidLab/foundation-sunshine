using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using Foundation.DualSense;
using Microsoft.Win32;

namespace Sunshine.Ds5Sidecar;

internal static class GameInputCommand
{
    // Same mutex as the pinned Core's RegistryLedger/OemNameOverrideStore.
    private const string LedgerMutex = @"Global\HIDMaestro-OEM-Recovery";
    private static readonly string BackupRoot = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "FoundationSunshine", "DualSenseGameInput");

    internal static int Run(string action, string? backupId, string? resultPath)
    {
        GameInputResult result;
        try
        {
            if (action is not ("check" or "repair" or "restore") || (action == "restore") != (backupId != null))
                throw new ArgumentException("Use --gameinput check|repair|restore; only restore requires --backup-id <id>");
            if (action != "check")
            {
                using var identity = WindowsIdentity.GetCurrent();
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException("Administrator privileges are required to repair or restore mappings");
                PrepareDirectory();
            }
            // Checking is read-only and never creates a mutex or a backup directory.
            using var mutex = action == "check" ? null : new Mutex(false, LedgerMutex);
            var acquired = false;
            try
            {
                if (mutex != null)
                {
                    try { acquired = mutex.WaitOne(TimeSpan.FromSeconds(10)); }
                    catch (AbandonedMutexException) { acquired = true; }
                    if (!acquired) throw new IOException("HIDMaestro is busy; retry when all controller programs are idle");
                }
                var engine = new GameInputRepair(Registry.LocalMachine, BackupRoot, EnsureIdle,
                    validateFile: path => ValidateBackupSecurity(new FileInfo(path).GetAccessControl()), secureFile: SecureBackupFile);
                result = action switch
                {
                    "check" => engine.Check(), "repair" => engine.Repair(), _ => engine.Restore(backupId!),
                };
            }
            finally { if (acquired) mutex!.ReleaseMutex(); }
        }
        catch (UnauthorizedAccessException error) { result = new("access_denied", backupId, false, [], error.Message); }
        catch (Exception error) { result = new("operation_failed", backupId, false, [], error.Message); }
        var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };
        var json = JsonSerializer.Serialize(result, options);
        if (System.Text.Encoding.UTF8.GetByteCount(json) > 48 * 1024)
        {
            result = result with { Mapping = null, Detail = result.Detail ?? "Custom mapping exceeds the inline report limit; it was preserved" };
            json = JsonSerializer.Serialize(result, options);
        }
        Console.WriteLine(json);
        if (resultPath != null) File.WriteAllText(resultPath, json + Environment.NewLine);
        return result.State is "inspection_failed" or "access_denied" or "operation_failed" or "repair_failed" or "recovery_required" ? 1 : 0;
    }

    private static void PrepareDirectory()
    {
        // No user-writable parent or junction may redirect an elevated backup/restore.
        var parent = Directory.GetParent(BackupRoot)!;
        foreach (var path in new[] { parent.FullName, BackupRoot })
        {
            if (!Directory.Exists(path))
            {
                var acl = new DirectorySecurity();
                acl.SetAccessRuleProtection(true, false);
                var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
                acl.SetOwner(admins);
                foreach (var sid in new[] { admins, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
                    acl.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                        InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
                acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
                    FileSystemRights.ReadAndExecute, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                    PropagationFlags.None, AccessControlType.Allow));
                FileSystemAclExtensions.Create(new DirectoryInfo(path), acl);
            }
            var info = new DirectoryInfo(path);
            if ((info.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Backup directory cannot be a junction or symbolic link");
            ValidateBackupSecurity(info.GetAccessControl());
        }
    }

    internal static void ValidateBackupSecurity(FileSystemSecurity security)
    {
        var owner = security.GetOwner(typeof(SecurityIdentifier)) as SecurityIdentifier
            ?? throw new IOException("Backup directory owner could not be read");
        bool Trusted(SecurityIdentifier sid) => sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) || sid.IsWellKnown(WellKnownSidType.LocalSystemSid);
        if (!Trusted(owner)) throw new IOException("Backup directory must be owned by Administrators or SYSTEM");
        if (new RawSecurityDescriptor(security.GetSecurityDescriptorBinaryForm(), 0).DiscretionaryAcl == null)
            throw new IOException("Backup permissions cannot contain a null DACL");
        const FileSystemRights write = FileSystemRights.Write | FileSystemRights.Delete | FileSystemRights.DeleteSubdirectoriesAndFiles
            | FileSystemRights.ChangePermissions | FileSystemRights.TakeOwnership;
        foreach (FileSystemAccessRule rule in security.GetAccessRules(true, true, typeof(SecurityIdentifier)))
            if (rule.AccessControlType == AccessControlType.Allow && (rule.FileSystemRights & write) != 0
                && !Trusted((SecurityIdentifier)rule.IdentityReference))
                throw new IOException("Backup directory grants write access outside Administrators and SYSTEM");
    }

    private static void SecureBackupFile(string path)
    {
        var security = new FileSecurity();
        security.SetAccessRuleProtection(true, false);
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        security.SetOwner(admins);
        foreach (var sid in new[] { admins, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
            FileSystemRights.Read, AccessControlType.Allow));
        new FileInfo(path).SetAccessControl(security);
        ValidateBackupSecurity(new FileInfo(path).GetAccessControl());
    }

    private static void EnsureIdle()
    {
        foreach (var process in Process.GetProcessesByName("Sunshine.Ds5Sidecar"))
        {
            using (process) if (process.Id != Environment.ProcessId) throw new IOException("Close active DualSense sidecar sessions before repair");
        }
        foreach (var path in new[] {
            @"SOFTWARE\HIDMaestroRegistryLedger\GameInput\054C0CE600010005\Holders",
            @"SOFTWARE\HIDMaestroRegistryLedger\JoystickOem\VID_054C&PID_0CE6\Holders" })
        {
            using var holders = Registry.LocalMachine.OpenSubKey(path);
            if (holders == null) continue;
            foreach (var holder in holders.GetValueNames())
            {
                var parts = holder.Split('-');
                if (parts.Length != 3 || !int.TryParse(parts[0], out var pid)
                    || !long.TryParse(parts[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var start))
                    throw new IOException("Unrecognized HIDMaestro ownership record; repair refused");
                try
                {
                    using var process = Process.GetProcessById(pid);
                    if (process.StartTime.ToFileTimeUtc() == start) throw new IOException("A live program holds this controller mapping");
                }
                catch (ArgumentException) { /* process is gone */ }
            }
        }
        // CfgMgr32 enumerates present devices without loading a HID driver or installing anything.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (CM_Get_Device_ID_List_SizeW(out var length, null, 0x100) != 0 || length > 4 * 1024 * 1024)
                throw new IOException("Cannot establish whether the DualSense is unplugged");
            var buffer = new char[length];
            var status = CM_Get_Device_ID_ListW(null, buffer, length, 0x100);
            if (status == 0x1a) continue; // CR_BUFFER_SMALL: device list changed
            if (status != 0) throw new IOException("Unable to enumerate present devices");
            if (new string(buffer).Split('\0', StringSplitOptions.RemoveEmptyEntries)
                .Any(id => id.Contains("VID_054C&PID_0CE6", StringComparison.OrdinalIgnoreCase)))
                throw new IOException("Unplug the DualSense and end virtual DualSense sessions, then retry");
            return;
        }
        throw new IOException("Device enumeration changed repeatedly; retry when devices are idle");
    }

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint CM_Get_Device_ID_List_SizeW(out uint length, string? filter, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint CM_Get_Device_ID_ListW(string? filter, [Out] char[] buffer, uint length, uint flags);
}
