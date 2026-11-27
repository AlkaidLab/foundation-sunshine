namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- only an install that administrators alone can change is run as administrator ----
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

        // without trust in the config, the working directory alone names the game
        var game = Path.Combine(scratch, "trust", "game");
        Directory.CreateDirectory(Path.Combine(game, "Game"));
        Same<Profile?>(null, InstallTrust.GameIn(game, "p"), "no known executable: nothing is started");
        File.WriteAllText(Path.Combine(game, "Game", "StarRail.exe"), "x");
        Same("starRail", InstallTrust.GameIn(game, "p")?.Game, "the one known executable one level under the game's folder");
        Directory.CreateDirectory(Path.Combine(game, "Other"));
        File.WriteAllText(Path.Combine(game, "Other", "YuanShen.exe"), "x");
        Same<Profile?>(null, InstallTrust.GameIn(game, "p"), "two games: none is chosen");

        // end to end: an install the person can change runs nothing of itself, and its config is not read
        var before = Environment.CurrentDirectory;
        var noticeDir = Path.Combine(scratch, "trust", "notice");
        try
        {
            Environment.CurrentDirectory = Path.Combine(scratch, "trust");
            Launcher.InstallBlocker = InstallTrust.Blocker;
            Same(Path.Combine(scratch, "host"), InstallTrust.HostRoot(Path.Combine(scratch, "host", "tools") + "\\"), "the host's folder is above tools");
            using var log = RunLog.Open([Path.Combine(scratch, "trust", "log")], () => new DateTime(2026, 11, 27, 9, 0, 0));
            string? Env(string name) => name == LaunchNotice.DirEnv ? noticeDir : null;
            var code = Launcher.RunAsync(["--sparxie", Path.Combine(mine, "Sparxie.exe"), "--profile", "genshin-cn-1"], log, Env, scratch).GetAwaiter().GetResult();
            Same(ExitCodes.Unsupported, code, "no game in the working directory: the untrusted install ends the launch without reading its config");
        }
        finally
        {
            Environment.CurrentDirectory = before;
        }
    }
}
