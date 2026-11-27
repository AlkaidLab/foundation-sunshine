using System.Security.AccessControl;
using System.Security.Principal;

namespace Sparxie.Launch;

/// <summary>
/// Whether the Sparxie install may run with administrator rights: only when administrators alone can change it. Otherwise any program of the signed-in person could swap
/// Sparxie.Broker.exe (or a DLL it loads) and be run as administrator by the next launch. The host applies the same rule (host_admin_only_tree, install_platform.cpp);
/// the launcher checks again right before it would start anything of the install, because the folder can have changed since the application was saved.
/// </summary>
internal static class InstallTrust
{
    private const int WriteData = 0x2, AppendData = 0x4, WriteExtended = 0x10, DeleteChild = 0x40, WriteAttributes = 0x100;
    private const int Delete = 0x10000, WriteDac = 0x40000, WriteOwner = 0x80000, GenericAll = 0x10000000, GenericWrite = 0x40000000;

    private const int Change = WriteData | AppendData | WriteExtended | WriteAttributes | DeleteChild | Delete | WriteDac | WriteOwner | GenericWrite | GenericAll;
    // above the folder only what swaps an existing entry matters: the root of C: lets every user add a folder of their own, which replaces nothing
    private const int Swap = DeleteChild | Delete | WriteDac | WriteOwner | GenericAll;
    private const int MaxEntries = 20000;

    /// <summary>SYSTEM, BUILTIN\Administrators, NT SERVICE\TrustedInstaller.</summary>
    private static bool Administrative(SecurityIdentifier? sid) =>
        sid?.Value is "S-1-5-18" or "S-1-5-32-544" or "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    /// <summary>The folder an install is made of: the one that holds Sparxie.exe (above app-*).</summary>
    public static string Root(string sparxiePath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(sparxiePath)) ?? sparxiePath;
        return Path.GetFileName(dir).StartsWith("app-", StringComparison.OrdinalIgnoreCase) ? Path.GetDirectoryName(dir) ?? dir : dir;
    }

    /// <summary>The host's install folder, from the folder this launcher is in (the host ships it in tools).</summary>
    public static string HostRoot(string launcherDirectory)
    {
        var dir = Path.TrimEndingDirectorySeparator(Path.GetFullPath(launcherDirectory));
        return string.Equals(Path.GetFileName(dir), "tools", StringComparison.OrdinalIgnoreCase) ? Path.GetDirectoryName(dir) ?? dir : dir;
    }

    /// <summary>
    /// Null when only administrators can change the install; else the first path that someone else can (or that cannot be checked). For an install inside
    /// <paramref name="hostRoot"/> the folders above that one are not looked at: whoever could swap them could swap the host's own programs.
    /// </summary>
    public static string? Blocker(string sparxiePath, string? hostRoot = null)
    {
        var current = sparxiePath;
        try
        {
            var folder = new DirectoryInfo(Root(sparxiePath));
            current = folder.FullName;
            var drive = Path.GetPathRoot(folder.FullName);
            if (string.IsNullOrEmpty(drive) || drive.StartsWith(@"\\", StringComparison.Ordinal) || new DriveInfo(drive).DriveType != DriveType.Fixed) return current;
            if (!ClosedToOthers(folder, Change)) return current;
            var count = 0;
            var pending = new Stack<DirectoryInfo>();
            pending.Push(folder);
            var every = new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false, RecurseSubdirectories = false };
            while (pending.Count > 0)
            {
                foreach (var entry in pending.Pop().EnumerateFileSystemInfos("*", every))
                {
                    current = entry.FullName;
                    // a link fails inside ClosedToOthers, so the walk never follows one
                    if (++count > MaxEntries || !ClosedToOthers(entry, Change)) return current;
                    if (entry is DirectoryInfo child) pending.Push(child);
                }
            }
            bool IsHostRoot(DirectoryInfo d) => !string.IsNullOrEmpty(hostRoot) &&
                string.Equals(Path.TrimEndingDirectorySeparator(d.FullName), Path.TrimEndingDirectorySeparator(Path.GetFullPath(hostRoot)), StringComparison.OrdinalIgnoreCase);
            if (IsHostRoot(folder)) return null;
            for (var above = folder.Parent; above is not null; above = above.Parent)
            {
                current = above.FullName;
                // nobody deletes or renames the root of a drive, so the DELETE a data drive grants on it replaces nothing
                if (!ClosedToOthers(above, above.Parent is null ? Swap & ~Delete : Swap)) return current;
                if (IsHostRoot(above)) return null;
            }
            return null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException or NotSupportedException or InvalidOperationException)
        {
            return current;
        }
    }

    private static bool ClosedToOthers(FileSystemInfo entry, int writes)
    {
        // the root of a drive reports itself as hidden and system, never as a link
        if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) return false;
        FileSystemSecurity security = entry is DirectoryInfo directory
            ? directory.GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access)
            : ((FileInfo)entry).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access);
        var descriptor = new RawSecurityDescriptor(security.GetSecurityDescriptorBinaryForm(), 0);
        // no DACL at all (also what a file system without permissions answers) lets everyone do everything
        if (!Administrative(descriptor.Owner) || descriptor.DiscretionaryAcl is not { } acl) return false;
        foreach (var ace in acl)
        {
            if ((ace.AceFlags & AceFlags.InheritOnly) != 0) continue;
            if (ace is not CommonAce { IsCallback: false } common) return false;  // conditional and object grants are not interpreted
            if (common.AceQualifier == AceQualifier.AccessDenied) continue;
            if (common.AceQualifier != AceQualifier.AccessAllowed) return false;
            if ((common.AccessMask & writes) != 0 && !Administrative(common.SecurityIdentifier)) return false;
        }
        return true;
    }

    /// <summary>The known executables of the titles that can start through Sparxie, with the game id each one has.</summary>
    private static readonly (string Name, string Game)[] KnownGames =
    [
        ("YuanShen.exe", "genshin"), ("GenshinImpact.exe", "genshin"), ("StarRail.exe", "starRail"),
        ("ZenlessZoneZero.exe", "zenlessZoneZero"), ("ZenlessZoneZeroBeta.exe", "zenlessZoneZero"), ("Endfield.exe", "endfield"),
    ];

    /// <summary>
    /// The game of an application whose Sparxie may not be trusted: its config is as open as the install, so nothing in it decides what runs as administrator. The
    /// working directory Sunshine gave the application (the game's folder, from the host's own application list) does: the one known game executable in it or one level
    /// under it. Null when there is none, or more than one.
    /// </summary>
    public static Profile? GameIn(string workingDirectory, string profileId)
    {
        try
        {
            if (!Directory.Exists(workingDirectory)) return null;
            List<(string Path, string Game)> Under(IEnumerable<string> folders) =>
                folders.SelectMany(f => KnownGames.Select(k => (Path: System.IO.Path.Combine(f, k.Name), k.Game))).Where(c => File.Exists(c.Path)).ToList();
            var found = Under([workingDirectory]);
            if (found.Count == 0) found = Under(Directory.GetDirectories(workingDirectory));
            return found.Count == 1 ? new Profile(profileId, profileId, found[0].Game, found[0].Path, null) : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }
}
