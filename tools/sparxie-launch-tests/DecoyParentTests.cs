using System.Diagnostics;
using System.Reflection;

namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- the Endfield decoy-parent launch (the Collapse Launcher plugin's method) ----
    private static void DecoyParentChecks(string scratch)
    {
        // the command line of the host's own Endfield direct start
        Check(EndfieldDirect.IsRequested(["--endfield-direct", @"C:\G\Endfield.exe"]), "the direct-start flag is recognised");
        Check(!EndfieldDirect.IsRequested(["--sparxie", "x", "--profile", "a"]), "a Sparxie command line is not a direct start");
        var parsed = EndfieldDirect.Parse(["--endfield-direct", @"C:\G\y\..\Endfield.exe", "--parent-source", @"C:\G\Other.exe"], out var err);
        Check(parsed is not null && err == "", "the direct-start command line parses");
        Same(@"C:\G\Endfield.exe", parsed?.Exe, "the game path is normalised");
        Same(@"C:\G\Other.exe", parsed?.ParentSource, "the parent source");
        Same(null, EndfieldDirect.Parse(["--endfield-direct=C:\\G\\Endfield.exe"], out _)?.ParentSource, "no parent source by default");
        Check(EndfieldDirect.Parse(["--endfield-direct"], out err) is null && err.Contains("needs a value"), "a flag without a value is refused");
        Check(EndfieldDirect.Parse(["--endfield-direct", "  "], out err) is null && err.Contains("path"), "a blank path is refused");
        Check(EndfieldDirect.Parse(["--endfield-direct", "C:\\G\\Endfield.exe", "--extra"], out err) is null && err.Contains("--extra"), "an unknown argument is refused");
        // the game's arguments follow a "--" separator: the host's screen plan, nothing else
        Same("", parsed?.GameArguments, "no separator: no game arguments");
        const string plan = "-screen-width 1920 -screen-height 1080 -screen-fullscreen 0 -popupwindow";
        var withArgs = EndfieldDirect.Parse(["--endfield-direct", @"C:\G\Endfield.exe", "--", "-screen-width", "1920", "-screen-height", "1080", "-screen-fullscreen", "0", "-popupwindow"], out err);
        Check(withArgs is not null && err == "", "a command line with the separator parses");
        Same(plan, withArgs?.GameArguments, "everything after the separator is the game's arguments, in order");
        Same(@"C:\G\Endfield.exe", withArgs?.Exe, "the game path is unchanged by the separator");
        var withSource = EndfieldDirect.Parse(["--endfield-direct=C:\\G\\Endfield.exe", "--parent-source", @"C:\G\Other.exe", "--", "-screen-fullscreen", "1"], out _);
        Check(withSource?.ParentSource == @"C:\G\Other.exe" && withSource?.GameArguments == "-screen-fullscreen 1", "the parent source before the separator and the arguments after it");
        Same("", EndfieldDirect.Parse(["--endfield-direct", @"C:\G\Endfield.exe", "--"], out _)?.GameArguments, "a bare separator is no arguments");
        foreach (var bad in new[] { new[] { "--evil" }, new[] { "-screen-width", "abc" }, new[] { "-screen-width", "99999" }, new[] { "-screen-width", "10" }, new[] { "-screen-fullscreen", "2" }, new[] { "-screen-height" }, new[] { "-batchmode" }, new[] { "-screen-width", "1920", "&", "calc" } })
            Check(EndfieldDirect.Parse(["--endfield-direct", @"C:\G\Endfield.exe", "--", .. bad], out err) is null && err.Contains("game arguments"), $"not a screen argument: {string.Join(' ', bad)}");
        Check(DirectLaunch.UsesDecoyParent(new Profile("e", "e", "endfield", "x", null)) && !DirectLaunch.UsesDecoyParent(new Profile("s", "s", "starRail", "x", null)), "only Endfield is started under a decoy parent");

        // where the decoy's image comes from
        Same(@"C:\G\PlatformProcess.exe", DecoyParent.ResolveSource(@"C:\G\Endfield.exe", null), "the default source is PlatformProcess.exe in the game's folder");
        Same(@"C:\G\sub\X.exe", DecoyParent.ResolveSource(@"C:\G\Endfield.exe", @"sub\X.exe"), "a relative source is relative to the game's folder");
        Same(@"D:\X.exe", DecoyParent.ResolveSource(@"C:\G\Endfield.exe", @"D:\X.exe"), "an absolute source is kept");

        var root = Path.Combine(scratch, "decoy");
        Directory.CreateDirectory(root);
        string Stage(string name) => Path.Combine(root, name, "_Hi3_Hg", "parent");
        bool NothingStaged(string name) => !Directory.Exists(Stage(name)) || !Directory.EnumerateFiles(Stage(name), "*", SearchOption.AllDirectories).Any();

        // failure paths: nothing is started and nothing stays behind
        var f1 = Path.Combine(root, "missing");
        Directory.CreateDirectory(f1);
        File.WriteAllText(Path.Combine(f1, "Endfield.exe"), "game");
        var missing = Assert<FileNotFoundException>(() => DecoyParent.Create(Path.Combine(f1, "Endfield.exe"), null));
        Check(missing is not null, "a missing PlatformProcess.exe is refused");
        File.WriteAllText(Path.Combine(f1, "notexe.dll"), "x");
        Check(Assert<FileNotFoundException>(() => DecoyParent.Create(Path.Combine(f1, "Endfield.exe"), "notexe.dll")) is not null, "a source that is not an .exe is refused");
        var f2 = Path.Combine(root, "unsigned");
        Directory.CreateDirectory(f2);
        File.WriteAllText(Path.Combine(f2, "Endfield.exe"), "game");
        File.WriteAllText(Path.Combine(f2, "PlatformProcess.exe"), "platform");
        Check(Assert<InvalidDataException>(() => DecoyParent.Create(Path.Combine(f2, "Endfield.exe"), null)) is not null, "unsigned images fail the signature check");
        Check(NothingStaged("missing") && NothingStaged("unsigned"), "a refused decoy leaves no staged file");

        // a stale session folder of a dead launcher is swept, our own and non-numeric ones stay
        var f3 = Path.Combine(root, "sweep");
        Directory.CreateDirectory(Path.Combine(f3, "_Hi3_Hg", "parent", "2000000000", "aaaa"));
        File.WriteAllText(Path.Combine(f3, "_Hi3_Hg", "parent", "2000000000", "aaaa", Path.GetFileName(DecoyParent.ImageName)), "old");
        Directory.CreateDirectory(Path.Combine(f3, "_Hi3_Hg", "parent", Environment.ProcessId.ToString(), "bbbb"));
        Directory.CreateDirectory(Path.Combine(f3, "_Hi3_Hg", "parent", "notapid", "cccc"));
        DecoyParent.SweepStale(f3);
        Check(!Directory.Exists(Path.Combine(f3, "_Hi3_Hg", "parent", "2000000000")), "a dead owner's session folder is swept");
        Check(Directory.Exists(Path.Combine(f3, "_Hi3_Hg", "parent", Environment.ProcessId.ToString(), "bbbb")) && Directory.Exists(Path.Combine(f3, "_Hi3_Hg", "parent", "notapid", "cccc")), "a live owner's and a foreign folder stay");

        // the production signature check with a real Authenticode image, when this machine has one
        var signed = new[] { @"C:\Program Files\dotnet\dotnet.exe", @"C:\Program Files\Git\cmd\git.exe" }.FirstOrDefault(File.Exists);
        if (signed is not null)
        {
            var f4 = Path.Combine(root, "signed");
            Directory.CreateDirectory(f4);
            File.Copy(signed, Path.Combine(f4, "Endfield.exe"));
            File.Copy(signed, Path.Combine(f4, "PlatformProcess.exe"));
            if (DecoyParent.HasMatchingSigner(Path.Combine(f4, "Endfield.exe"), Path.Combine(f4, "Endfield.exe")))
            {
                using (var decoy = DecoyParent.Create(Path.Combine(f4, "Endfield.exe"), null))
                {
                    Check(decoy.Pid > 0 && File.Exists(decoy.StagedPath) && Path.GetFileName(decoy.StagedPath) == "Games.exe", "same-signer images are accepted and staged as Games.exe");
                    using var p = Process.GetProcessById(decoy.Pid);
                    Check(!p.HasExited, "the decoy is a live (suspended) process");
                }
                Check(NothingStaged("signed"), "the session folder is removed with the decoy");
                File.Copy(Path.Combine(f2, "PlatformProcess.exe"), Path.Combine(f4, "Unsigned.exe"));
                Check(Assert<InvalidDataException>(() => DecoyParent.Create(Path.Combine(f4, "Endfield.exe"), "Unsigned.exe")) is not null, "an unsigned source is refused next to a signed game");
            }
        }

        // end to end with the dummy game (a GUI-subsystem program that records its parent)
        var fakeDir = typeof(Tests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().FirstOrDefault(a => a.Key == "FakeGameDir")?.Value;
        if (fakeDir is null || !File.Exists(Path.Combine(fakeDir, "FakeGame.exe")))
        {
            Check(false, $"FakeGame.exe was not built (looked in '{fakeDir}')");
            return;
        }
        DummyGameRuns(root, fakeDir);
    }

    private static string ReadShared(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }

    private static T? Assert<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T ex) { return ex; }
        return null;
    }

    private static string NewGameDir(string root, string name, string fakeDir)
    {
        var dir = Path.Combine(root, name);
        Directory.CreateDirectory(dir);
        foreach (var file in new[] { "FakeGame.dll", "FakeGame.runtimeconfig.json" }) File.Copy(Path.Combine(fakeDir, file), Path.Combine(dir, file), true);
        File.Copy(Path.Combine(fakeDir, "FakeGame.exe"), Path.Combine(dir, "Endfield.exe"), true);
        File.Copy(Path.Combine(fakeDir, "FakeGame.exe"), Path.Combine(dir, "PlatformProcess.exe"), true);
        return dir;
    }

    private static Dictionary<string, string> ReadRecord(string path)
    {
        for (var i = 0; i < 200 && !File.Exists(path); i++) Thread.Sleep(50);
        return File.Exists(path) ? File.ReadAllLines(path).Select(l => l.Split('=', 2)).Where(p => p.Length == 2).ToDictionary(p => p[0], p => p[1]) : [];
    }

    private static bool Gone(int pid)
    {
        for (var i = 0; i < 100; i++)
        {
            try { using var p = Process.GetProcessById(pid); if (p.HasExited) return true; }
            catch (ArgumentException) { return true; }
            Thread.Sleep(50);
        }
        return false;
    }

    private static void DummyGameRuns(string root, string fakeDir)
    {
        // A: DecoyParent + JobGuard: the dummy's parent is Games.exe; closing the job ends both
        var a = NewGameDir(root, "e2e-job", fakeDir);
        var record = Path.Combine(a, "record.txt");
        Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", record);
        Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", "60000");
        try
        {
            var job = JobGuard.Create();
            var decoy = DecoyParent.Create(Path.Combine(a, "Endfield.exe"), null, verifySigner: false);
            var game = decoy.StartGame(Path.Combine(a, "Endfield.exe"), "-screen-width 1920 -screen-height 1080", a);
            Check(job.TryAssign(decoy.Pid) is null && job.TryAssign(game.Id) is null, "the decoy and the game are in the job");
            var seen = ReadRecord(record);
            Same(game.Id.ToString(), seen.GetValueOrDefault("self_pid"), "the dummy ran as the created process");
            Same(decoy.Pid.ToString(), seen.GetValueOrDefault("parent_pid"), "the dummy's parent is the decoy");
            Same(decoy.StagedPath.ToLowerInvariant(), (seen.GetValueOrDefault("parent_image") ?? "").ToLowerInvariant(), "the parent's image is the staged Games.exe");
            Check(Path.GetFileName(seen.GetValueOrDefault("parent_image") ?? "") == "Games.exe" && decoy.StagedPath.StartsWith(Path.Combine(a, "_Hi3_Hg", "parent"), StringComparison.OrdinalIgnoreCase), "Games.exe is in the plugin's session folder");
            Check((seen.GetValueOrDefault("cmdline") ?? "").Contains("-screen-width 1920 -screen-height 1080"), "the game's arguments follow its path");
            Same(a.ToLowerInvariant(), (seen.GetValueOrDefault("cwd") ?? "").ToLowerInvariant(), "the game runs in its own folder");
            var gamePid = game.Id;
            var decoyPid = decoy.Pid;
            game.Dispose();
            job.Dispose(); // kill on close: the session ends before it is running
            Check(Gone(gamePid) && Gone(decoyPid), "closing the job ends the game and the decoy together");
            decoy.Dispose();
            Check(!Directory.EnumerateFiles(Path.Combine(a, "_Hi3_Hg", "parent"), "*", SearchOption.AllDirectories).Any(), "the session folder is removed");
        }
        finally
        {
            Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", null);
        }

        const string plan = "-screen-width 1920 -screen-height 1080 -screen-fullscreen 0 -popupwindow";
        // B: the launcher's direct start of an Endfield profile; the game ends by itself, then the decoy and its folder go
        var b = NewGameDir(root, "e2e-direct", fakeDir);
        record = Path.Combine(b, "record.txt");
        Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", record);
        Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", "1500");
        try
        {
            var logDir = Path.Combine(root, "e2e-direct-log");
            using var log = RunLog.Open([logDir], () => new DateTime(2026, 11, 12, 9, 0, 0));
            var profile = new Profile("endfield-direct", "Endfield", "endfield", Path.Combine(b, "Endfield.exe"), null);
            var code = DirectLaunch.RunAsync(profile, log, true, b, null, verifySigner: false, gameArguments: plan).GetAwaiter().GetResult();
            Same(0, code, "the direct start follows the game's exit code");
            var seen = ReadRecord(record);
            Check((seen.GetValueOrDefault("cmdline") ?? "").EndsWith($" {plan}", StringComparison.Ordinal), "under the decoy parent the screen arguments end the game's command line (a managed dummy shows its .dll as the program)");
            var parentImage = seen.GetValueOrDefault("parent_image") ?? "";
            Check(Path.GetFileName(parentImage) == "Games.exe" && parentImage.StartsWith(Path.Combine(b, "_Hi3_Hg", "parent"), StringComparison.OrdinalIgnoreCase), "the direct start gave the game a Games.exe parent");
            Check(int.TryParse(seen.GetValueOrDefault("parent_pid"), out var parentPid) && Gone(parentPid), "the decoy ended with the game");
            Check(!Directory.Exists(Path.Combine(b, "_Hi3_Hg", "parent")) || !Directory.EnumerateFiles(Path.Combine(b, "_Hi3_Hg", "parent"), "*", SearchOption.AllDirectories).Any(), "its session folder is gone");
            var text = ReadShared(log.Path!);
            Check(text.Contains("decoy parent ready") && text.Contains("created with the decoy parent"), "the decoy launch is logged");
        }
        finally
        {
            Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", null);
            Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", null);
        }

        // C: with the signature check on (production), unsigned images fall back to the plain start and the log says why
        var c = NewGameDir(root, "e2e-fallback", fakeDir);
        record = Path.Combine(c, "record.txt");
        Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", record);
        Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", "500");
        try
        {
            using var log = RunLog.Open([Path.Combine(root, "e2e-fallback-log")], () => new DateTime(2026, 11, 12, 9, 1, 0));
            var profile = new Profile("endfield-direct", "Endfield", "endfield", Path.Combine(c, "Endfield.exe"), null);
            Same(0, DirectLaunch.RunAsync(profile, log, true, c, gameArguments: "-screen-fullscreen 1").GetAwaiter().GetResult(), "the plain start runs the game");
            var seen = ReadRecord(record);
            Check((seen.GetValueOrDefault("cmdline") ?? "").Contains("-screen-fullscreen 1"), "the plain fallback start carries the screen arguments too");
            Check(!(seen.GetValueOrDefault("parent_image") ?? "").EndsWith("Games.exe", StringComparison.OrdinalIgnoreCase), "no decoy parent without valid signatures");
            Check(ReadShared(log.Path!).Contains("the decoy parent method failed"), "the downgrade is logged");
        }
        finally
        {
            Environment.SetEnvironmentVariable("ECL_FAKE_RECORD", null);
            Environment.SetEnvironmentVariable("ECL_FAKE_LIFE_MS", null);
        }
    }
}
