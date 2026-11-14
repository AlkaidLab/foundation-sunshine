using System.Text.Json;
namespace Sparxie.Launch;

internal static partial class Tests
{
    private static int _checks;
    private static readonly List<string> Failures = [];

    private static int Main()
    {
        var scratch = Path.Combine(Path.GetTempPath(), "sparxie-launch-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(scratch);
        try
        {
            ParseArguments();
            ExitCodeMapping();
            BrokerArguments();
            BrokerConsole();
            ProtoRoundTrip();
            GrpcFraming();
            ConfigLoading(scratch);
            LogFile(scratch);
            LogRotation(scratch);
            LogFallback(scratch);
            ProfileWire();
            NamesAndStages();
            PureTouchRules(scratch);
            SparxieLogCopy(scratch);
            NoticeFile(scratch);
            PlanRules();
            AdoptRunningGame(scratch);
            DirectStart(scratch);
            DecoyParentChecks(scratch);
            TouchUiSwitch(scratch);
            EndfieldFields();
            ScreenArguments(scratch);
            StageReports(scratch);
            CrashReports(scratch);
        }
        catch (Exception ex)
        {
            Failures.Add($"unexpected {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            try { Directory.Delete(scratch, true); } catch (IOException) { }
        }
        foreach (var f in Failures) Console.Error.WriteLine("FAIL: " + f);
        Console.WriteLine($"{_checks} checks, {Failures.Count} failed");
        return Failures.Count == 0 ? 0 : 1;
    }

    private static void Check(bool ok, string what)
    {
        _checks++;
        if (!ok) Failures.Add(what);
    }

    private static void Same<T>(T expected, T actual, string what) =>
        Check(EqualityComparer<T>.Default.Equals(expected, actual), $"{what}: expected '{expected}', got '{actual}'");

    // ---- argument parsing: the contract the host's touch_command writes ----
    private static void ParseArguments()
    {
        var ok = LaunchOptions.Parse(["--sparxie", @"C:\Program Files\Sunshine\config\components\sparxie\Sparxie.exe", "--profile", "starrail-cn-1"], out var err);
        Check(ok is not null && err == "", "the host's command line parses");
        Same(@"C:\Program Files\Sunshine\config\components\sparxie\Sparxie.exe", ok?.SparxiePath, "sparxie path");
        Same("starrail-cn-1", ok?.ProfileId, "profile id");
        var eq = LaunchOptions.Parse(["--profile=zzz-cn-1", "--sparxie=C:\\x\\Sparxie.exe"], out _);
        Same("zzz-cn-1", eq?.ProfileId, "--name=value form");
        Check(LaunchOptions.Parse(["--sparxie", "C:\\x\\Sparxie.exe"], out err) is null && err.Contains("--profile"), "a missing profile is named");
        Check(LaunchOptions.Parse(["--profile", "a"], out err) is null && err.Contains("--sparxie"), "a missing sparxie path is named");
        Check(LaunchOptions.Parse(["--profile"], out err) is null && err.Contains("needs a value"), "a flag without a value is refused");
        Check(LaunchOptions.Parse(["--sparxie", "C:\\x\\Sparxie.exe", "--profile", "a", "--extra"], out err) is null && err.Contains("--extra"), "an unknown argument is refused");
        Check(LaunchOptions.Parse([], out err) is null && err.Contains("usage"), "no arguments shows the usage");
        Check(LaunchOptions.Parse(["--sparxie", "  ", "--profile", "a"], out _) is null, "a blank path is refused");
        Same(@"C:\x\Sparxie.exe", LaunchOptions.Parse(["--sparxie", @"C:\x\y\..\Sparxie.exe", "--profile", "a"], out _)?.SparxiePath, "the path is normalised");
    }

    private static void ExitCodeMapping()
    {
        Same(0, ExitCodes.ForState("Exited"), "Exited is success");
        Same(7, ExitCodes.ForState("Failed"), "Failed");
        Same(7, ExitCodes.ForState("HostCrashedBeforeRunning"), "HostCrashedBeforeRunning");
        Same(7, ExitCodes.ForState("HostCrashedAfterRunning"), "HostCrashedAfterRunning");
        Check(!ExitCodes.IsTerminal("Running") && !ExitCodes.IsTerminal("Starting") && !ExitCodes.IsTerminal(""), "Starting and Running are not final");
        Check(new[] { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10 }.Distinct().Count() == 10, "exit codes are distinct");
    }

    // ---- what the Broker expects on its command line ----
    private static void BrokerArguments()
    {
        Same("--pipe-name=sparxie-abc --parent-pid=1234", BrokerProcess.Arguments("sparxie-abc", 1234), "broker arguments");
        Check(BrokerProcess.IsValidPipeName(BrokerProcess.NewPipeName()), "a generated pipe name is valid");
        Check(!BrokerProcess.IsValidPipeName("bad name") && !BrokerProcess.IsValidPipeName("a\\b") && !BrokerProcess.IsValidPipeName("") && !BrokerProcess.IsValidPipeName(new string('a', 129)), "invalid pipe names are refused");
        Check(Throws<ArgumentException>(() => BrokerProcess.Arguments("bad name", 1)), "arguments refuse a bad pipe name");
        Check(Throws<ArgumentOutOfRangeException>(() => BrokerProcess.Arguments("ok", 0)), "arguments refuse pid 0");
    }

    private static void BrokerConsole()
    {
        Check(!BrokerProcess.ShowsLine("info: Microsoft.Hosting.Lifetime[14]", true), "routine info lines are dropped");
        Check(!BrokerProcess.ShowsLine("      Now listening on: http://pipe:/x", false), "their continuation lines are dropped too");
        Check(BrokerProcess.ShowsLine("fail: Sparxie.Broker[0]", false), "failures are kept");
        Check(BrokerProcess.ShowsLine("      System.Exception: boom", true), "their continuation lines are kept");
        Check(BrokerProcess.ShowsLine("Broker only accepts one --pipe-name argument", false), "plain text (an argument error) is kept");
    }

    private static bool Throws<T>(Action action) where T : Exception
    {
        try { action(); return false; } catch (T) { return true; }
    }

    // ---- the wire ----
    private static void ProtoRoundTrip()
    {
        var bytes = Proto.Message((1, Proto.Int(1)), (2, Proto.String("req")), (3, Proto.Bool(true)), (4, Proto.Int(-1)));
        var f = Proto.Parse(bytes);
        Same(1, Proto.Int(f, 1), "int field");
        Same("req", Proto.String(f, 2), "string field");
        Check(Proto.Bool(f, 3), "bool field");
        Same(-1, Proto.Int(f, 4), "negative int32 survives (10-byte varint)");
        Same("", Proto.String(f, 9), "an absent string is empty");
        Same(0, Proto.Int(f, 9), "an absent int is zero");
        // field 1 = int 1 is "08 01"; field 2 = "ab" is "12 02 61 62"
        Check(Proto.Message((1, Proto.Int(1)), (2, Proto.String("ab"))).SequenceEqual(new byte[] { 0x08, 0x01, 0x12, 0x02, 0x61, 0x62 }), "known bytes");
        var nested = Proto.Append(Proto.Message((1, Proto.String("id"))), 5, Proto.Message((2, Proto.Int(300))));
        var inner = Proto.Parse(Proto.Parse(nested).First(x => x.Number == 5).Bytes);
        Same(300, Proto.Int(inner, 2), "a two-byte varint inside a nested message");
        // fixed-width fields of a newer Broker are skipped, not fatal
        var withFixed = new byte[] { 0x0D, 1, 2, 3, 4, 0x11, 1, 2, 3, 4, 5, 6, 7, 8, 0x18, 0x07 };
        Same(7, Proto.Int(Proto.Parse(withFixed), 3), "fixed-width fields are skipped");
        Check(Throws<InvalidDataException>(() => Proto.Parse(new byte[] { 0x12, 0x09, 0x61 })), "a field longer than the message is refused");
        Check(Throws<EndOfStreamException>(() => Proto.Parse(new byte[] { 0x08, 0x80 })), "a cut varint is refused");
        var start = StartResponse.Parse(Proto.Message((1, Proto.Bool(true)), (2, Proto.String("sess")), (3, Proto.Int(2)), (4, Proto.Int(0)), (5, Proto.String("ok"))));
        Check(start.Accepted && start.SessionId == "sess" && start.Stage == 2 && start.Message == "ok", "StartSessionResponse parses");
        var ev = SessionEvent.Parse(Proto.Message((1, Proto.String("sess")), (2, Proto.String("Running")), (3, Proto.Int(5))));
        Check(ev.State == "Running" && ev.SessionId == "sess" && ev.Stage == 5, "SessionEvent parses");
    }

    private static void GrpcFraming()
    {
        var frame = Grpc.Frame(new byte[] { 1, 2, 3 });
        Check(frame.SequenceEqual(new byte[] { 0, 0, 0, 0, 3, 1, 2, 3 }), "gRPC frame header");
        using var ms = new MemoryStream(frame.Concat(Grpc.Frame([])).ToArray());
        var one = Grpc.ReadFrameAsync(ms, CancellationToken.None).GetAwaiter().GetResult();
        var two = Grpc.ReadFrameAsync(ms, CancellationToken.None).GetAwaiter().GetResult();
        var end = Grpc.ReadFrameAsync(ms, CancellationToken.None).GetAwaiter().GetResult();
        Check(one!.SequenceEqual(new byte[] { 1, 2, 3 }) && two is { Length: 0 } && end is null, "two frames then a clean end");
        using var cut = new MemoryStream(new byte[] { 0, 0, 0, 0, 9, 1 });
        Check(Throws<EndOfStreamException>(() => Grpc.ReadFrameAsync(cut, CancellationToken.None).GetAwaiter().GetResult()), "a cut frame is an error");
        using var big = new MemoryStream(new byte[] { 0, 0xFF, 0xFF, 0xFF, 0xFF });
        Check(Throws<InvalidDataException>(() => Grpc.ReadFrameAsync(big, CancellationToken.None).GetAwaiter().GetResult()), "an oversized frame is refused");
    }

    // ---- config.json ----
    private static void ConfigLoading(string scratch)
    {
        var local = @"C:\Users\u\AppData\Local";
        Check(SparxieConfig.DataDirectories(@"C:\s\sparxie\app-0.1.0\Sparxie.exe", null, local).SequenceEqual([@"C:\s\sparxie", local + @"\Sparxie"]), "data dirs: above app-*, then LocalAppData");
        Check(SparxieConfig.DataDirectories(@"C:\s\sparxie\Sparxie.exe", null, null).SequenceEqual([@"C:\s\sparxie"]), "data dirs: beside the shell");
        Check(SparxieConfig.DataDirectories(@"C:\s\sparxie\Sparxie.exe", @"D:\data", local).SequenceEqual([@"D:\data"]), "SPARXIE_DATA_DIR wins alone");
        var withConfig = Path.Combine(scratch, "second"); Directory.CreateDirectory(withConfig); File.WriteAllText(Path.Combine(withConfig, "config.json"), "{}");
        Same(withConfig, SparxieConfig.PickDataDirectory([Path.Combine(scratch, "first"), withConfig]), "the first directory that has a config.json");
        Same(Path.Combine(scratch, "none"), SparxieConfig.PickDataDirectory([Path.Combine(scratch, "none"), Path.Combine(scratch, "none2")]), "no config anywhere: the first is named");

        var config = Path.Combine(scratch, "config.json");
        File.WriteAllText(config, """
        {"schemaVersion":1,"profiles":[
          {"id":"a","displayName":"原神触屏","game":"genshin","executablePath":"C:\\G\\YuanShen.exe",
           "hoyo":{"fpsUnlockEnabled":false,"targetFps":144,"backgroundFpsLimitEnabled":false,"backgroundFps":15,"processPriority":"high",
                   "genshinFollowInGamePreset":true,"genshinPreset30Fps":45,"genshinPreset60Fps":90,"genshinTouchUiScaleOverrideEnabled":true,"genshinTouchUiScalePercent":250}},
          {"id":"z","displayName":"绝区零","game":"zenlessZoneZero","executablePath":"C:\\G\\ZenlessZoneZero.exe","hoyo":null},
          {"id":"s","displayName":"星铁","game":"starRail","executablePath":"C:\\G\\StarRail.exe","hoyo":{}}
        ]}
        """);
        var a = SparxieConfig.Load(config, "a");
        Check(a.Game == "genshin" && a.DisplayName == "原神触屏" && a.ExecutablePath == @"C:\G\YuanShen.exe", "profile fields");
        var h = a.Hoyo!;
        Check(!h.FpsUnlock && h.TargetFps == 144 && !h.BackgroundLimit && h.BackgroundFps == 15 && h.Priority == "high", "fps and priority fields reach the profile");
        Check(h.FollowPreset && h.Preset30 == 45 && h.Preset60 == 90 && h.ScaleOverride && h.Scale == 250, "preset mapping and touch UI scale reach the profile");
        Check(SparxieConfig.Load(config, "z").Hoyo is null, "null hoyo stays null");
        var s = SparxieConfig.Load(config, "s").Hoyo!;
        Check(s.FpsUnlock && s.TargetFps == 120 && s.BackgroundFps == 10 && s.Priority == "normal" && s.Scale == 400, "missing hoyo fields take Sparxie's defaults");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(config, "nope"), "not found: nope"), "an unknown profile is named");
        Check(Throws<FileNotFoundException>(() => SparxieConfig.Load(Path.Combine(scratch, "missing.json"), "a")), "a missing config is reported");
        var bad = Path.Combine(scratch, "bad.json");
        File.WriteAllText(bad, "{ not json");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(bad, "a"), "not valid JSON"), "broken JSON is reported");
        File.WriteAllText(bad, "{\"x\":1}");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(bad, "a"), "no profiles"), "no profiles list is reported");

        // Sparxie moved a config aside and started a new one: the launcher says so instead of only "profile not found"
        var moved = Path.Combine(scratch, "moved");
        Directory.CreateDirectory(moved);
        var movedConfig = Path.Combine(moved, "config.json");
        File.WriteAllText(movedConfig, "{\"schemaVersion\":1,\"selectedProfileId\":null,\"profiles\":[]}");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "not found: endfield-cn-1") &&
              !ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "config.invalid"), "no moved-aside file: the plain message");
        File.WriteAllText(Path.Combine(moved, "config.invalid-20261112095137-d8b536fa.json"), "{\"profiles\":[{\"id\": \"endfield-cn-1\"}]}");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "config.invalid-20261112095137-d8b536fa.json") &&
              ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "still has this profile"), "a set-aside config that has the profile is named");
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "other"), "does not have this profile"), "a set-aside config without the profile says so");
        File.WriteAllText(Path.Combine(moved, "config.invalid-20261113000000-aaaaaaaa.json"), "{}");
        File.SetLastWriteTimeUtc(Path.Combine(moved, "config.invalid-20261112095137-d8b536fa.json"), new DateTime(2026, 11, 12, 9, 51, 37, DateTimeKind.Utc));
        File.SetLastWriteTimeUtc(Path.Combine(moved, "config.invalid-20261113000000-aaaaaaaa.json"), new DateTime(2026, 11, 13, 0, 0, 0, DateTimeKind.Utc));
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "config.invalid-20261113000000-aaaaaaaa.json"), "the newest set-aside file is the one named");
        File.WriteAllText(Path.Combine(moved, "config.repaired-20261114000000-bbbbbbbb.json"), "{\"profiles\":[{\"id\":\"endfield-cn-1\"}]}");
        File.SetLastWriteTimeUtc(Path.Combine(moved, "config.repaired-20261114000000-bbbbbbbb.json"), new DateTime(2026, 11, 14, 0, 0, 0, DateTimeKind.Utc));
        Check(ThrowsMessage<InvalidDataException>(() => SparxieConfig.Load(movedConfig, "endfield-cn-1"), "config.repaired-20261114000000-bbbbbbbb.json"), "a repaired-config copy is named too");
    }

    private static bool ThrowsMessage<T>(Action action, string part) where T : Exception
    {
        try { action(); return false; } catch (T ex) { return ex.Message.Contains(part, StringComparison.Ordinal); }
    }

    // ---- the log ----
    private static void LogFile(string scratch)
    {
        var dir = Path.Combine(scratch, "logs1");
        var clock = new DateTime(2026, 11, 11, 20, 5, 9, 123);
        string path;
        using (var log = RunLog.Open([dir], () => clock))
        {
            path = log.Path!;
            log.Info("first");
            clock = clock.AddMilliseconds(250);
            log.Warn("second\r\nline two");
            log.Error("third");
        }
        Same("sparxie-launch-20261111-200509.log", Path.GetFileName(path), "log file name");
        var lines = File.ReadAllLines(path);
        Same(3, lines.Length, "one line per message (line breaks are folded)");
        Same("2026-11-11 20:05:09.123 INFO  first", lines[0], "timestamped info line");
        Same("2026-11-11 20:05:09.373 WARN  second | line two", lines[1], "folded warn line");
        Check(lines[2].EndsWith("ERROR third", StringComparison.Ordinal), "error line");
        // the same second again gets its own file
        using var again = RunLog.Open([dir], () => new DateTime(2026, 11, 11, 20, 5, 9));
        Check(again.Path != path && File.Exists(again.Path!), "two runs in one second do not share a file");
    }

    private static void LogRotation(string scratch)
    {
        var dir = Path.Combine(scratch, "logs2");
        Directory.CreateDirectory(dir);
        for (var i = 0; i < 27; i++)
            File.WriteAllText(Path.Combine(dir, $"sparxie-launch-20261001-{i:000000}.log"), "old");
        File.WriteAllText(Path.Combine(dir, "sunshine.log"), "not ours");
        using var log = RunLog.Open([dir], () => new DateTime(2026, 11, 11, 1, 2, 3));
        var ours = Directory.GetFiles(dir, "sparxie-launch-*.log");
        Same(RunLog.Keep, ours.Length, "the last 20 logs are kept");
        Check(File.Exists(log.Path!), "the new log is kept");
        Check(File.Exists(Path.Combine(dir, "sparxie-launch-20261001-000026.log")) && !File.Exists(Path.Combine(dir, "sparxie-launch-20261001-000000.log")), "the oldest go first");
        Check(File.Exists(Path.Combine(dir, "sunshine.log")), "other files are not touched");
    }

    private static void LogFallback(string scratch)
    {
        // a blocked first folder (a file where the folder should be) falls through to the next
        var blocked = Path.Combine(scratch, "blocked");
        File.WriteAllText(blocked, "a file");
        var good = Path.Combine(scratch, "good");
        using (var log = RunLog.Open([Path.Combine(blocked, "logs"), good]))
            Check(log.Path is not null && log.Path.StartsWith(good, StringComparison.Ordinal), "an unusable folder falls back to the next");
        using (var none = RunLog.Open([Path.Combine(blocked, "x")]))
        {
            Check(none.Path is null, "no usable folder gives a silent log");
            none.Info("goes nowhere, does not throw");
        }

        var env = new Dictionary<string, string> { ["LOCALAPPDATA"] = @"C:\Users\u\AppData\Local" };
        string? Env(string k) => env.GetValueOrDefault(k);
        var dirs = RunLog.CandidateDirectories(@"C:\Program Files\Sunshine\tools\", Env).ToList();
        Same(@"C:\Program Files\Sunshine\config\logs", dirs[0], "the host log folder is config\\logs beside tools\\");
        Same(@"C:\Users\u\AppData\Local\Sunshine\logs", dirs[1], "then LocalAppData");
        env[RunLog.LogDirEnv] = @"D:\logs";
        Same(@"D:\logs", RunLog.CandidateDirectories(@"C:\Program Files\Sunshine\tools", Env).First(), "SPARXIE_LAUNCH_LOG_DIR comes first");
        Check(!RunLog.CandidateDirectories(@"C:\elsewhere", Env).Any(d => d.EndsWith(@"config\logs", StringComparison.Ordinal)), "outside a tools folder there is no config\\logs guess");
    }

    // ---- a session without the touch interface (HoyoSettings.touch_ui_enabled) ----
    private static void TouchUiSwitch(string scratch)
    {
        foreach (var touch in new[] { true, false })
        {
            var hoyo = new HoyoSettings(true, 120, true, 10, "normal", false, 60, 1000, false, 400, touch);
            var payload = BrokerRpc.StartSessionPayload(new Profile("p", "n", "starRail", @"C:\S\StarRail.exe", hoyo), "r");
            var profile = Proto.Parse(Proto.Parse(payload).First(x => x.Number == 3).Bytes);
            var fields = Proto.Parse(profile.First(x => x.Number == 5).Bytes);
            var sent = fields.FirstOrDefault(x => x.Number == 11);
            Check(sent.Number == 11 && sent.Wire == 0, $"touch_ui_enabled (field 11) is always sent (touch={touch})");
            Same(touch, Proto.Bool(fields, 11), $"touch_ui_enabled value on the wire (touch={touch})");
        }

        // the profile file: absent means on; false means a PC-interface session
        var config = Path.Combine(scratch, "touchui.json");
        File.WriteAllText(config, """
            { "schemaVersion": 1, "profiles": [
              { "id": "old", "displayName": "old", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe", "hoyo": { "fpsUnlockEnabled": true, "targetFps": 120 } },
              { "id": "touch", "displayName": "touch", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe", "hoyo": { "fpsUnlockEnabled": true, "targetFps": 120, "touchUiEnabled": true } },
              { "id": "pc", "displayName": "pc", "game": "genshin", "executablePath": "C:\\G\\YuanShen.exe", "hoyo": { "fpsUnlockEnabled": true, "targetFps": 144, "touchUiEnabled": false } } ] }
            """);
        Same(true, SparxieConfig.Load(config, "old").Hoyo!.TouchUi, "a profile without the field keeps the touch interface");
        Same(true, SparxieConfig.Load(config, "touch").Hoyo!.TouchUi, "touchUiEnabled true");
        Same(false, SparxieConfig.Load(config, "pc").Hoyo!.TouchUi, "touchUiEnabled false");
        Check(SparxieConfig.Load(config, "pc").Hoyo!.Describe().Contains("touchUi=False"), "the log line names the interface");

        // the Ping answer lists the Broker's features (field 3, repeated)
        var withCap = PingInfo.Parse(Proto.Message((1, Proto.Int(1)), (2, Proto.String("0.2.0")), (3, Proto.String("touch-ui-switch")), (3, Proto.String("other"))));
        Check(withCap.Has(PingInfo.TouchUiSwitch) && withCap.Capabilities.Count == 2, "Ping lists capabilities");
        var bare = PingInfo.Parse(Proto.Message((1, Proto.Int(1)), (2, Proto.String("0.2.0"))));
        Check(!bare.Has(PingInfo.TouchUiSwitch) && bare.ToString().Contains("capabilities none"), "an older Broker lists none");
        Same(11, ExitCodes.Unsupported, "exit code for an unsupported feature");
    }

    // ---- an Endfield session carries the client's device, the stream's size and the refresh rate (fields 12-16 of the settings) ----
    private static void EndfieldFields()
    {
        var hoyoJson = JsonDocument.Parse("""{ "fpsUnlockEnabled": true, "targetFps": 144, "backgroundFpsLimitEnabled": true, "backgroundFps": 10, "processPriority": "normal", "endfieldInputMode": "touch", "endfieldAutoResolution": true, "endfieldWidth": 0, "endfieldHeight": 0, "endfieldRefreshRate": 0 }""").RootElement;
        var profile = new Profile("endfield-cn-1", "终末地", "endfield", @"C:\E\Endfield.exe", HoyoSettings.FromJson(hoyoJson));
        List<ProtoField> SettingsOf(Profile p) => Proto.Parse(Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(p, "r")).First(x => x.Number == 3).Bytes).First(x => x.Number == 5).Bytes);
        static string? None(string _) => null;

        // a touch app on a 2388x1668 stream at 120 fps: touch input, the stream's size, 120 Hz
        var resolved = EndfieldSession.Resolve(profile, name => name switch { "SUNSHINE_CLIENT_WIDTH" => "2388", "SUNSHINE_CLIENT_HEIGHT" => "1668", "SUNSHINE_CLIENT_FPS" => "120", _ => null });
        var f = SettingsOf(resolved);
        Same("touch", Proto.String(f, 12), "Endfield input of a touch client");
        Same(false, Proto.Bool(f, 13), "the stream's size is sent, not the automatic resolution");
        Same(2388, Proto.Int(f, 14), "Endfield width on the wire");
        Same(1668, Proto.Int(f, 15), "Endfield height on the wire");
        Same(120, Proto.Int(f, 16), "Endfield refresh rate on the wire");
        Same(144, Proto.Int(f, 2), "the target frame rate (field 2) is the app's own");
        Check(resolved.Hoyo!.Describe().Contains("endfieldInput=touch"), "the log line names the resolved input");

        // the device of the client no longer decides: only the person's choice does, touch or pc, and nothing is automatic
        f = SettingsOf(EndfieldSession.Resolve(profile with { Hoyo = profile.Hoyo! with { EndfieldInput = "pc" } }, name => name == "SUNSHINE_SOMETHING" ? "x" : null));
        Same("pc", Proto.String(f, 12), "Endfield input of a keyboard, mouse and gamepad app");
        foreach (var stored in new[] { "auto", "" })
        {
            f = SettingsOf(EndfieldSession.Resolve(profile with { Hoyo = profile.Hoyo! with { EndfieldInput = stored } }, None));
            Same("touch", Proto.String(f, 12), $"a stored '{stored}' (the removed automatic input) is touch");
        }
        // no stream size: the resolution stays automatic
        f = SettingsOf(EndfieldSession.Resolve(profile, None));
        Same("touch", Proto.String(f, 12), "the input is the app's own");
        Same(true, Proto.Bool(f, 13), "without a stream size the resolution stays automatic");
        Same(0, Proto.Int(f, 14), "no width without a stream size");

        // the person's own choices win over the stream: a fixed input, a fixed size, a fixed refresh rate
        var fixedJson = JsonDocument.Parse("""{ "endfieldInputMode": "pc", "endfieldAutoResolution": false, "endfieldWidth": 1920, "endfieldHeight": 1080, "endfieldRefreshRate": 165 }""").RootElement;
        var chosen = new Profile("e", "e", "endfield", @"C:\E\Endfield.exe", HoyoSettings.FromJson(fixedJson));
        var cf = SettingsOf(EndfieldSession.Resolve(chosen, name => name == "SUNSHINE_CLIENT_WIDTH" ? "2388" : name == "SUNSHINE_CLIENT_HEIGHT" ? "1668" : "60"));
        Same("pc", Proto.String(cf, 12), "a fixed input stays");
        Same(1920, Proto.Int(cf, 14), "a fixed size is not replaced by the stream's");
        Same(165, Proto.Int(cf, 16), "a fixed refresh rate is not replaced by the stream's");

        // the other games send none of these fields
        var star = SettingsOf(EndfieldSession.Resolve(new Profile("s", "s", "starRail", @"C:\S\StarRail.exe", HoyoSettings.FromJson(hoyoJson)), None));
        Check(!star.Any(x => x.Number >= 12), "a Star Rail session has no Endfield fields");

        // the Ping lists the Broker's Endfield support
        var ping = PingInfo.Parse(Proto.Message((1, Proto.Int(1)), (2, Proto.String("0.1.0-preview.9-test.1")), (3, Proto.String("touch-ui-switch")), (3, Proto.String("endfield"))));
        Check(ping.Has(PingInfo.Endfield) && ping.Has(PingInfo.TouchUiSwitch), "the build's capabilities are read");
    }

    // ---- the StartSession request carries every setting ----
    private static void ProfileWire()
    {
        var hoyo = new HoyoSettings(false, 144, false, 15, "high", true, 45, 90, true, 250);
        var payload = BrokerRpc.StartSessionPayload(new Profile("a", "名", "genshin", @"C:\G\YuanShen.exe", hoyo), "req1");
        var top = Proto.Parse(payload);
        Check(Proto.Int(top, 1) == 1 && Proto.String(top, 2) == "req1", "protocol version and request id");
        var profile = Proto.Parse(top.First(x => x.Number == 3).Bytes);
        Check(Proto.String(profile, 1) == "a" && Proto.String(profile, 2) == "名" && Proto.String(profile, 3) == "genshin" && Proto.String(profile, 4) == @"C:\G\YuanShen.exe", "profile identity fields");
        var f = Proto.Parse(profile.First(x => x.Number == 5).Bytes);
        Check(!Proto.Bool(f, 1) && Proto.Int(f, 2) == 144 && !Proto.Bool(f, 3) && Proto.Int(f, 4) == 15 && Proto.String(f, 5) == "high", "fps unlock, target, background limit, priority");
        Check(Proto.Bool(f, 6) && Proto.Int(f, 7) == 45 && Proto.Int(f, 8) == 90 && Proto.Bool(f, 9) && Proto.Int(f, 10) == 250, "preset mapping and touch UI scale");
        var bare = Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(new Profile("z", "z", "zenlessZoneZero", @"C:\Z\ZenlessZoneZero.exe", null), "r")).First(x => x.Number == 3).Bytes);
        Check(bare.All(x => x.Number != 5), "no hoyo block is sent when the profile has none");
        Check(hoyo.Describe().Contains("targetFps=144") && hoyo.Describe().Contains("scale=250"), "the log line names the settings");
    }

    // ---- stage and error names ----
    private static void NamesAndStages()
    {
        Same("Cleanup(13)", SparxieNames.Stage(13), "stage 13");
        Same("Running(10)", SparxieNames.Stage(10), "stage 10");
        Same("stage 99", SparxieNames.Stage(99), "an unknown stage");
        Same("RuntimeLoadFailed(30)", SparxieNames.Error(30), "error 30");
        Same("GameAlreadyRunning(9)", SparxieNames.Error(9), "error 9");
        Same("Hoyo runtime error 6 = INSTALL_CONFIRM_FAILED", SparxieNames.HoyoCode("Hoyo 启动注入失败: 6 Hoyo 启动注入失败"), "the runtime's own code");
        Same("Hoyo runtime error 4 = SCAN_FAILED", SparxieNames.HoyoCode("Hoyo 启动注入失败: 4 Hoyo 启动注入失败"), "scan failed");
        Check(SparxieNames.HoyoCode("ZZZ Runtime 注入失败: 游戏在安装确认前退出") is null, "other messages carry no Hoyo code");
        Check(SparxieNames.HoyoCode("Hoyo 会话创建失败: 99 x") is null, "an unknown Hoyo code is not named");
    }

    // ---- touch only ----
    private static void PureTouchRules(string scratch)
    {
        var off = new Profile("a", "n", "genshin", @"C:\G\YuanShen.exe", new HoyoSettings(false, 120, true, 10, "high", true, 45, 90, true, 250));
        Check(PureTouch.IsTouchOnly(off), "unlock off on a Hoyo game is touch only");
        var forced = PureTouch.WithUnlock(off);
        Check(forced.Hoyo!.FpsUnlock && forced.Hoyo.TargetFps == PureTouch.GameOwnCap, "the unlock goes on at the game's own cap");
        Check(forced.Hoyo.Priority == "high" && forced.Hoyo.FollowPreset && forced.Hoyo.Preset30 == 45 && forced.Hoyo.Preset60 == 90 && forced.Hoyo.ScaleOverride && forced.Hoyo.Scale == 250 && forced.Hoyo.BackgroundFps == 10, "every other setting is kept");
        var on = off with { Hoyo = off.Hoyo! with { FpsUnlock = true, TargetFps = 144 } };
        Check(!PureTouch.IsTouchOnly(on) && PureTouch.WithUnlock(on) == on, "an unlock that is on is left alone");
        var zzz = new Profile("z", "z", "zenlessZoneZero", @"C:\Z\ZenlessZoneZero.exe", null);
        Check(!PureTouch.IsTouchOnly(zzz) && PureTouch.WithUnlock(zzz) == zzz, "Zenless Zone Zero has no frame-rate setting to change");
        var dir = Path.Combine(scratch, "app-0.1.0"); Directory.CreateDirectory(dir);
        var broker = Path.Combine(dir, "Sparxie.Broker.exe");
        Check(PureTouch.RuntimeHash(broker) is null, "no runtime beside the Broker: no hash");
        File.WriteAllBytes(Path.Combine(dir, "HoyoTouchCore.dll"), [1, 2, 3]);
        Same(64, PureTouch.RuntimeHash(broker)!.Length, "a SHA-256 is 64 hex characters");
        Check(PureTouch.KnownBrokenRuntimes.Contains("1a0897a111cf35ab07eb34a2b9a2be8d072221fa2f10a38923308ef89a0fb0a4"), "preview.9's runtime is on the list (case does not matter)");
    }

    // ---- Sparxie's own logs, copied on a failure ----
    private static void SparxieLogCopy(string scratch)
    {
        Check(!SparxieLogs.Keep("2026-11-11 20:11:08.631 [Information] Microsoft.Hosting.Lifetime: Application started."), "framework chatter is dropped");
        Check(SparxieLogs.Keep("2026-11-11 20:11:08.738 [Information] Sparxie.SessionHost.Services.HostService: session event"), "Sparxie's own lines are kept");
        Check(SparxieLogs.Keep("2026-11-11 20:11:08.738 [Warning] Microsoft.AspNetCore.Server.Kestrel: x"), "warnings are kept");
        Check(!SparxieLogs.Keep("   "), "blank lines are dropped");
        Check(!SparxieLogs.Keep("   at Grpc.Shared.Server.X.Invoke()") && !SparxieLogs.Keep("System.OperationCanceledException: The operation was canceled."), "the cancellation the launcher causes itself is dropped");
        Same(Path.Combine(@"C:\s\sparxie", "logs"), SparxieLogs.DirectoryFor(@"C:\s\sparxie\app-0.1.0\Sparxie.Broker.exe"), "the logs folder is above app-*");
        var root = Path.Combine(scratch, "sparxie"); var logs = Path.Combine(root, "logs"); Directory.CreateDirectory(logs);
        File.WriteAllText(Path.Combine(logs, "broker-20261111.log"), "old line\n");
        var copy = new SparxieLogs(logs);
        File.AppendAllText(Path.Combine(logs, "broker-20261111.log"), "[Information] Sparxie.Broker: StartSession accepted\n[Information] Microsoft.Hosting: noise\n");
        File.WriteAllText(Path.Combine(logs, "host-20261111.log"), "[Error] Sparxie.SessionHost: Hoyo failed: 6\n");
        var target = Path.Combine(scratch, "copylog"); string path;
        using (var log = RunLog.Open([target], () => new DateTime(2026, 11, 11, 1, 1, 1)))
        {
            path = log.Path!;
            copy.DumpNew(log);
        }
        var text = File.ReadAllText(path);
        Check(text.Contains("StartSession accepted") && text.Contains("Hoyo failed: 6"), "lines written after the snapshot are copied");
        Check(!text.Contains("old line") && !text.Contains("noise"), "older lines and chatter are not");
    }

    // ---- the note for the Web UI and the Panel ----
    private static void NoticeFile(string scratch)
    {
        var env = new Dictionary<string, string> { ["LOCALAPPDATA"] = @"C:\Users\u\AppData\Local" };
        string? Env(string k) => env.GetValueOrDefault(k);
        var dirs = LaunchNotice.CandidateDirectories(@"C:\Program Files\Sunshine\tools\", Env).ToList();
        Same(@"C:\Program Files\Sunshine\config\sparxie", dirs[0], "the note sits in the host's Sparxie data folder");
        Same(@"C:\Users\u\AppData\Local\Sunshine", dirs[1], "then LocalAppData");
        Check(LaunchNotice.Messages(LaunchNotice.KindDirect).Zh == "Sparxie 未能接管本次启动，已直接启动游戏", "the direct-start message is the specified one");
        Check(LaunchNotice.Messages(LaunchNotice.KindRetried).Zh.Length > 0 && LaunchNotice.Messages(LaunchNotice.KindUnlock).En.Contains("60"), "every kind has its message");
        var profile = new Profile("p1", "n", "starRail", @"C:\G\StarRail.exe", null);
        var json = LaunchNotice.Build(LaunchNotice.KindDirect, profile, "attempt 1: x", @"C:\logs\a.log", new DateTime(2026, 11, 11, 12, 0, 0, DateTimeKind.Utc));
        using var doc = System.Text.Json.JsonDocument.Parse(json);
        var root = doc.RootElement;
        Check(root.GetProperty("kind").GetString() == "direct" && root.GetProperty("profile").GetString() == "p1" && root.GetProperty("game").GetString() == "starRail"
            && root.GetProperty("time").GetString() == "2026-11-11T12:00:00Z" && root.GetProperty("version").GetInt32() == 1, "the note's fields");
        Check(root.GetProperty("message").GetString() == "Sparxie 未能接管本次启动，已直接启动游戏" && root.GetProperty("messageEn").GetString()!.StartsWith("Sparxie could not"), "the note carries both languages");
        Check(json.Contains("未能接管"), "Chinese is written as text, not escaped");
        var dir = Path.Combine(scratch, "notice");
        var blocked = Path.Combine(scratch, "blocked-file"); File.WriteAllText(blocked, "x");
        var where = LaunchNotice.Write([Path.Combine(blocked, "a"), dir], json);
        Check(where is not null && File.Exists(where) && Path.GetFileName(where) == LaunchNotice.FileName, "the note is written, past a folder that cannot be made");
        LaunchNotice.Clear([dir]);
        Check(!File.Exists(Path.Combine(dir, LaunchNotice.FileName)), "a clean launch removes the note");
    }

    // ---- what happens after an attempt ----
    private static void PlanRules()
    {
        Same(Next.Done, Plan.After(1, Outcome.Completed, false, true), "a finished session is done");
        Same(Next.Done, Plan.After(1, Outcome.Ended, false, true), "a game that ran and crashed is not started again");
        Same(Next.Retry, Plan.After(1, Outcome.NotStarted, false, true), "a session that never started is retried once");
        Same(Next.Fallback, Plan.After(2, Outcome.NotStarted, false, true), "after the retry the game is started directly");
        Same(Next.Fallback, Plan.After(1, Outcome.NotStarted, false, false), "no Broker: straight to the direct start");
        Same(Next.FollowRunningGame, Plan.After(1, Outcome.NotStarted, true, true), "a game that is already running is followed, never started twice");
        Same(Next.FollowRunningGame, Plan.After(2, Outcome.NotStarted, true, true), "also after the retry");
    }

    // ---- the direct start ----
    private static void DirectStart(string scratch)
    {
        Same(scratch, DirectLaunch.WorkingDirectory(scratch, @"C:\G\StarRail.exe"), "the working directory Sunshine gave the app");
        Same(@"C:\G", DirectLaunch.WorkingDirectory(@"Z:\no\such\dir", @"C:\G\StarRail.exe"), "else the game's folder");
        Same(System.Diagnostics.ProcessPriorityClass.High, DirectLaunch.PriorityClass("high"), "priority high");
        Same(System.Diagnostics.ProcessPriorityClass.BelowNormal, DirectLaunch.PriorityClass("belowNormal"), "priority belowNormal");
        Same(System.Diagnostics.ProcessPriorityClass.Normal, DirectLaunch.PriorityClass("anything else"), "priority default");
        var dir = Path.Combine(scratch, "direct"); string path;
        using (var log = RunLog.Open([dir], () => new DateTime(2026, 11, 11, 2, 2, 2)))
        {
            path = log.Path!;
            var missing = new Profile("m", "m", "starRail", Path.Combine(scratch, "nope", "StarRail.exe"), null);
            Same(ExitCodes.Config, DirectLaunch.RunAsync(missing, log, true, scratch).GetAwaiter().GetResult(), "a missing executable is a clear failure");
            // a real program that ends at once with an error code: the launcher's code follows the game's
            var where = Path.Combine(Environment.SystemDirectory, "where.exe");
            var code = DirectLaunch.RunAsync(new Profile("w", "w", "starRail", where, null), log, true, scratch).GetAwaiter().GetResult();
            Check(code != 0 && code != ExitCodes.Internal && code != ExitCodes.Config, "the exit code follows the game's");
        }
        var text = File.ReadAllText(path);
        Check(text.Contains("direct start: game started") && text.Contains("the game exited with code"), "the direct start is logged step by step");
    }

    // ---- the window and size the host chose, as Unity arguments of the game ----
    private static void ScreenArguments(string scratch)
    {
        var args = new[] { "--sparxie", @"C:\x\Sparxie.exe", "--profile", "starrail-cn-1" };
        Check(LaunchOptions.Parse(args, out _)?.Screen is null, "no screen flags, no screen arguments");

        var fixedWindow = LaunchOptions.Parse([.. args, "--screen-width", "1280", "--screen-height", "720", "--window-mode", "windowed"], out _);
        Same(new ScreenArgs(1280, 720, "windowed"), fixedWindow?.Screen, "a fixed window is read");
        Same("-screen-width 1280 -screen-height 720 -screen-fullscreen 0", fixedWindow?.Screen?.Arguments(), "the window text");
        var follow = LaunchOptions.Parse([.. args, "--screen-width=2868", "--screen-height=1320", "--window-mode=fullscreen"], out _);
        Same("-screen-width 2868 -screen-height 1320 -screen-fullscreen 1", follow?.Screen?.Arguments(), "following the stream: its size, full screen");
        Same("-screen-width 1920 -screen-height 1080 -screen-fullscreen 0 -popupwindow", new ScreenArgs(1920, 1080, "borderless").Arguments(), "borderless");
        Same("-screen-fullscreen 0", new ScreenArgs(0, 0, "windowed").Arguments(), "a mode without a size");
        Same("", new ScreenArgs(0, 0, "").Arguments(), "nothing asked, nothing sent");

        // values that cannot be used never stop a launch: they are dropped and named
        foreach (var bad in new[] { new[] { "--screen-width", "1280" }, new[] { "--screen-width", "abc", "--screen-height", "720" }, new[] { "--screen-width", "99999", "--screen-height", "720" },
            new[] { "--window-mode", "exclusive" }, new[] { "--screen-width", "-5", "--screen-height", "720" } })
        {
            var o = LaunchOptions.Parse([.. args, .. bad], out var err);
            Check(o is not null && err.Length == 0 && o.Screen is null && !string.IsNullOrEmpty(o.ScreenWarning), "an unusable screen value is dropped with a warning: " + string.Join(' ', bad));
        }
        Check(LaunchOptions.Parse([.. args, "--screen-width"], out var missing) is null && missing.Contains("needs a value"), "a screen flag without a value is refused like the others");

        // the wire: ScreenArgs screen = 6 { width 1, height 2, window_mode 3 }, only when asked
        var profile = new Profile("p", "n", "starRail", @"C:\S\StarRail.exe", null, new ScreenArgs(1280, 720, "windowed"));
        var nested = Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(profile, "r")).First(x => x.Number == 3).Bytes);
        var screen = Proto.Parse(nested.First(x => x.Number == 6).Bytes);
        Same(1280, Proto.Int(screen, 1), "wire width");
        Same(720, Proto.Int(screen, 2), "wire height");
        Same("windowed", Proto.String(screen, 3), "wire window mode");
        var plain = Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(profile with { Screen = null }, "r")).First(x => x.Number == 3).Bytes);
        Check(!plain.Any(x => x.Number == 6), "no screen object when nothing is asked");
        Same("screen-args", PingInfo.ScreenArgsCapability, "the capability name");
        Check(PingInfo.Parse(Proto.Message((1, Proto.Int(1)), (3, Proto.String("screen-args")))).Has(PingInfo.ScreenArgsCapability), "a Broker that lists it is understood");

        // the direct start (the safety net) puts the same text on the game's command line
        var dir = Path.Combine(scratch, "direct-screen"); string path;
        using (var log = RunLog.Open([dir], () => new DateTime(2026, 11, 12, 3, 3, 3)))
        {
            path = log.Path!;
            var where = Path.Combine(Environment.SystemDirectory, "where.exe");
            DirectLaunch.RunAsync(new Profile("w", "w", "starRail", where, null, new ScreenArgs(1280, 720, "windowed")), log, true, scratch).GetAwaiter().GetResult();
        }
        Check(File.ReadAllText(path).Contains("arguments -screen-width 1280 -screen-height 720 -screen-fullscreen 0"), "the direct start passes the screen arguments");
    }
}
