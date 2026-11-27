namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- the Broker is started as administrator only from an install that administrators alone can change ----
    private static void InstallTrustRules(string scratch)
    {
        var mine = Path.Combine(scratch, "trust", "sparxie");
        Directory.CreateDirectory(Path.Combine(mine, "app-0.2.1"));
        File.WriteAllText(Path.Combine(mine, "Sparxie.exe"), "x");
        Check(InstallTrust.Blocker(Path.Combine(mine, "Sparxie.exe")) is not null, "a folder of the signed-in person is not administrator-only");
        Same(mine, InstallTrust.Root(Path.Combine(mine, "app-0.2.1", "Sparxie.Desktop.exe")), "the install root is above app-*");
        // a small folder that Windows itself protects
        var system = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Windows NT", "TableTextService", "TableTextService.dll");
        Same<string?>(null, InstallTrust.Blocker(system), "a folder under Program Files is administrator-only");
        Check(InstallTrust.Blocker(@"\\server\share\Sparxie\Sparxie.exe") is not null, "a network path is refused");

        // end to end: for an install the person can change nothing is started, and its config is not read
        var noticeDir = Path.Combine(scratch, "trust", "notice");
        {
            Launcher.InstallBlocker = InstallTrust.Blocker;
            Same(Path.Combine(scratch, "host"), InstallTrust.HostRoot(Path.Combine(scratch, "host", "tools") + "\\"), "the host's folder is above tools");
            using var log = RunLog.Open([Path.Combine(scratch, "trust", "log")], () => new DateTime(2026, 11, 27, 9, 0, 0));
            string? Env(string name) => name == LaunchNotice.DirEnv ? noticeDir : null;
            var code = Launcher.RunAsync(["--sparxie", Path.Combine(mine, "Sparxie.exe"), "--profile", "genshin-cn-1"], log, Env, scratch).GetAwaiter().GetResult();
            Same(ExitCodes.Unprotected, code, "the launch ends before the config is read or anything of the install runs");
            Check(File.ReadAllText(Path.Combine(noticeDir, LaunchNotice.FileName)).Contains("\"kind\":\"unprotected\""), "the page is told why");
        }
    }
}
