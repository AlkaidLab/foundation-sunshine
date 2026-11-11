using System.Diagnostics;
using System.Reflection;
namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- a launch while the profile's game already runs (an orphan of a crashed host) joins it ----
    private static void AdoptRunningGame(string scratch)
    {
        Same(Start.AdoptRunningGame, Plan.AtStart(true), "a game that already runs is joined at the start");
        Same(Start.NewSession, Plan.AtStart(false), "no running game: a new session");

        // how a running process of the game's name is judged
        Same(BrokerProcess.ImageMatch.Same, BrokerProcess.Classify(@"c:\games\endfield\endfield.exe", @"C:\Games\Endfield\Endfield.exe"), "the same image, any case");
        Same(BrokerProcess.ImageMatch.Other, BrokerProcess.Classify(@"D:\Other\Endfield.exe", @"C:\Games\Endfield\Endfield.exe"), "the same name from another folder is another program");
        Same(BrokerProcess.ImageMatch.Unknown, BrokerProcess.Classify(null, @"C:\Games\Endfield\Endfield.exe"), "an unreadable image is unknown");
        Check(BrokerProcess.IsTheGame(BrokerProcess.ImageMatch.Unknown) && BrokerProcess.IsTheGame(BrokerProcess.ImageMatch.Same) && !BrokerProcess.IsTheGame(BrokerProcess.ImageMatch.Other),
            "unknown counts as the game (never a second copy), another folder does not");

        // the limited-information read and the lookup see this very process
        var self = Environment.ProcessPath!;
        Check(string.Equals(ProcessImage.TryPath(Environment.ProcessId), self, StringComparison.OrdinalIgnoreCase), "the image path of this process is read without MainModule");
        Check(BrokerProcess.FindGameProcesses(self).Contains(Environment.ProcessId), "this process is found by its own executable path");
        var elsewhere = Path.Combine(scratch, "elsewhere", Path.GetFileName(self));
        Check(!BrokerProcess.FindGameProcesses(elsewhere).Contains(Environment.ProcessId), "the same file name under another folder is not this process");

        // end to end: the fake game runs on its own; the launcher joins it, starts no Broker, leaves no notice and ends with the game
        var fakeDir = typeof(Tests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().FirstOrDefault(a => a.Key == "FakeGameDir")?.Value;
        if (fakeDir is null || !File.Exists(Path.Combine(fakeDir, "FakeGame.exe")))
        {
            Check(false, $"FakeGame.exe was not built (looked in '{fakeDir}')");
            return;
        }
        var root = Path.Combine(scratch, "adopt");
        var gameDir = NewGameDir(root, "game", fakeDir);
        var exe = Path.Combine(gameDir, "Endfield.exe");
        var sparxieDir = Path.Combine(root, "sparxie");
        Directory.CreateDirectory(sparxieDir);
        File.WriteAllText(Path.Combine(sparxieDir, "config.json"),
            "{\"schemaVersion\":1,\"profiles\":[{\"id\":\"endfield-cn-1\",\"displayName\":\"Endfield\",\"game\":\"endfield\",\"executablePath\":" +
            System.Text.Json.JsonSerializer.Serialize(exe) + ",\"hoyo\":null}]}");
        var noticeDir = Path.Combine(root, "notice");
        Directory.CreateDirectory(noticeDir);
        File.WriteAllText(Path.Combine(noticeDir, LaunchNotice.FileName), "{\"kind\":\"direct\"}");  // the failure note of an earlier launch

        Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", Path.Combine(gameDir, "record.txt"));
        Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", "4000");
        using var game = Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = gameDir, UseShellExecute = false })!;
        Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", null);
        Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", null);
        try
        {
            Check(BrokerProcess.FindGameProcesses(exe).Contains(game.Id) || WaitFound(exe, game.Id), "the dummy game is found by its path");
            using var log = RunLog.Open([Path.Combine(root, "log")], () => new DateTime(2026, 11, 12, 15, 0, 0));
            string? Env(string name) => name == LaunchNotice.DirEnv ? noticeDir : name == SparxieConfig.DataDirEnv ? sparxieDir : null;
            var clock = Stopwatch.StartNew();
            // no Sparxie.Broker.exe exists in the fake install: a launch that tried a session would fall back to the direct start instead of joining
            var code = Launcher.RunAsync(["--sparxie", Path.Combine(sparxieDir, "Sparxie.exe"), "--profile", "endfield-cn-1"], log, Env, root).GetAwaiter().GetResult();
            Same(ExitCodes.Exited, code, "joining a running game ends normally when the game ends");
            Check(clock.Elapsed > TimeSpan.FromMilliseconds(500), "the launcher stayed while the game ran");
            Check(game.HasExited, "the game ended by itself, the launcher did not start a second one");
            var text = log.Path is null ? "" : ReadShared(log.Path);
            Check(text.Contains("joining it"), "the log says the running game was joined");
            Check(!text.Contains("broker started") && !text.Contains("starting the game directly"), "no Broker and no direct start");
            Check(!File.Exists(Path.Combine(noticeDir, LaunchNotice.FileName)), "the failure note of an earlier launch is cleared: nothing failed");
        }
        finally
        {
            try { if (!game.HasExited) game.Kill(true); } catch (InvalidOperationException) { }
        }
    }

    private static bool WaitFound(string exe, int pid)
    {
        for (var i = 0; i < 40; i++)
        {
            if (BrokerProcess.FindGameProcesses(exe).Contains(pid)) return true;
            Thread.Sleep(100);
        }
        return false;
    }
}
