using System.Net.Http;
using System.Text;

namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- what the installed Sparxie supports: the Ping list, else capabilities.json, else the installed files ----
    private static void BrokerCapabilityRules(string scratch)
    {
        string NewInstall(string name, Action<string>? fill = null)
        {
            var dir = Path.Combine(scratch, "cap-" + name);
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "Sparxie.Broker.exe"), "");
            fill?.Invoke(dir);
            return Path.Combine(dir, "Sparxie.Broker.exe");
        }

        void Manifest(string dir, string json) => File.WriteAllText(Path.Combine(dir, "capabilities.json"), json);
        void Contracts(string dir, params string[] markers) => File.WriteAllBytes(Path.Combine(dir, "Sparxie.Contracts.dll"), Encoding.ASCII.GetBytes("MZ......" + string.Join("\0", markers) + "\0"));
        void EndfieldLibraries(string dir) { File.WriteAllText(Path.Combine(dir, "EndfieldCore.dll"), ""); File.WriteAllText(Path.Combine(dir, "EndfieldRuntime.dll"), ""); }
        static PingInfo Ping(params string[] capabilities) => new(1, "0.2.0", capabilities);

        // a Broker that lists its features is believed, whatever the files say
        var listed = BrokerCapabilities.Resolve(Ping("endfield"), NewInstall("ping", d => { Manifest(d, """{ "schema": 1, "capabilities": ["endfield","touch-ui-switch"] }"""); EndfieldLibraries(d); }));
        Same(CapabilitySource.Ping, listed.Source, "a Ping list wins");
        Check(listed.Has("endfield") && !listed.Has("touch-ui-switch"), "the Ping list is exact: what it leaves out is missing");

        // a Broker that lists nothing (older build) with a capabilities.json: the manifest
        var withManifest = BrokerCapabilities.Resolve(Ping(), NewInstall("manifest", d => Manifest(d, """{ "schema": 1, "capabilities": ["touch-ui-switch", "endfield", "screen-args"] }""")));
        Same(CapabilitySource.Manifest, withManifest.Source, "no Ping list: capabilities.json");
        Check(withManifest.Has("endfield") && withManifest.Has("touch-ui-switch") && withManifest.Has("screen-args"), "the manifest names are read");

        // a manifest that lists nothing is an answer too (preview.9 style): nothing is inferred behind it
        var emptyManifest = BrokerCapabilities.Resolve(Ping(), NewInstall("empty", d => { Manifest(d, """{ "schema": 1, "capabilities": [] }"""); EndfieldLibraries(d); }));
        Same(CapabilitySource.Manifest, emptyManifest.Source, "an empty manifest is read");
        Same(0, emptyManifest.Names.Count, "an empty manifest means no features");

        // no manifest: the installed files. The 1116 case: Endfield libraries but a protocol without the touch switch or screen fields
        var legacyEndfield = BrokerCapabilities.Resolve(Ping(), NewInstall("endfield-only", d => { EndfieldLibraries(d); Contracts(d, "EndfieldWidth", "ProfileSnapshot"); }));
        Same(CapabilitySource.InstalledFiles, legacyEndfield.Source, "no manifest: the files");
        Check(legacyEndfield.Has("endfield") && !legacyEndfield.Has("touch-ui-switch") && !legacyEndfield.Has("screen-args"), "Endfield libraries alone give Endfield only");

        var legacyFull = BrokerCapabilities.Resolve(null, NewInstall("full", d => { EndfieldLibraries(d); Contracts(d, "TouchUiEnabled", "ScreenArgs"); }));
        Check(legacyFull.Has("endfield") && legacyFull.Has("touch-ui-switch") && legacyFull.Has("screen-args"), "the protocol markers give the touch switch and the screen arguments");

        var onlyOneLibrary = BrokerCapabilities.Resolve(Ping(), NewInstall("one-library", d => File.WriteAllText(Path.Combine(d, "EndfieldCore.dll"), "")));
        Check(!onlyOneLibrary.Has("endfield"), "one Endfield library is not Endfield support");

        var bare = BrokerCapabilities.Resolve(Ping(), NewInstall("bare"));
        Same(0, bare.Names.Count, "an install with no Endfield files and no protocol markers has no features");

        // an unreadable or foreign manifest is no manifest: the files answer
        foreach (var bad in new[] { "{ not json", """{ "schema": 2, "capabilities": ["endfield"] }""", """{ "schema": 1, "capabilities": "endfield" }""", """{ "schema": 1, "capabilities": [1] }""", "[]" })
        {
            var r = BrokerCapabilities.Resolve(Ping(), NewInstall("bad-" + Math.Abs(bad.GetHashCode()), EndfieldLibraries));
            Check(r.Source == CapabilitySource.InstalledFiles && r.Has("endfield"), $"a bad manifest falls back to the files: {bad}");
        }

        Check(BrokerCapabilities.Resolve(Ping(), Path.Combine(scratch, "no-such-dir", "Sparxie.Broker.exe")).Names.Count == 0, "a missing install folder has no features");
        Check(listed.ToString().Contains("reported by the Broker") && withManifest.ToString().Contains("capabilities.json") && legacyFull.ToString().Contains("installed files"), "the log line says where the list came from");

        // the decisions of the launcher
        static Profile P(string game, bool touchUi = true, ScreenArgs? screen = null) =>
            new("p", "p", game, @"C:\G\Game.exe", new HoyoSettings(true, 120, false, 10, "normal", false, 60, 60, false, 100, TouchUi: touchUi), screen);
        var warnings = new List<string>();
        var endfieldProfile = P("endfield");
        Same(null, Launcher.CheckSupport(ref endfieldProfile, legacyEndfield, warnings.Add), "an Endfield session on an older build that ships the libraries starts");
        var endfieldRefused = P("endfield");
        Check(Launcher.CheckSupport(ref endfieldRefused, bare, warnings.Add)?.Contains("endfield capability") == true, "an Endfield session on a build without Endfield is refused");
        var pcProfile = P("starRail", touchUi: false);
        Check(Launcher.CheckSupport(ref pcProfile, legacyEndfield, warnings.Add)?.Contains("touch-ui-switch") == true, "a PC-interface session on a build without the switch is refused");
        var pcOk = P("starRail", touchUi: false);
        Same(null, Launcher.CheckSupport(ref pcOk, withManifest, warnings.Add), "a PC-interface session on a build with the switch starts");
        var screenProfile = P("genshin", screen: new ScreenArgs(2388, 1668, ScreenArgs.Fullscreen));
        Same(null, Launcher.CheckSupport(ref screenProfile, legacyEndfield, warnings.Add), "screen arguments do not stop a session");
        Check(screenProfile.Screen is null && warnings.Count == 1, "screen arguments are dropped with one warning when the build cannot carry them");
        var screenKept = P("genshin", screen: new ScreenArgs(2388, 1668, ScreenArgs.Fullscreen));
        Same(null, Launcher.CheckSupport(ref screenKept, withManifest, warnings.Add), "screen arguments on a build that carries them");
        Check(screenKept.Screen is not null, "the screen arguments are kept");
    }

    // ---- a real package: start its Broker, read its Ping answer. Runs only when SPARXIE_TEST_BROKER_EXE names a Broker and the tests run elevated (no UAC prompt is ever raised) ----
    private static void PackagedBrokerReports(string scratch)
    {
        var exe = Environment.GetEnvironmentVariable("SPARXIE_TEST_BROKER_EXE");
        if (string.IsNullOrEmpty(exe) || !File.Exists(exe) || !BrokerProcess.IsElevated()) return;
        using var log = RunLog.Open([Path.Combine(scratch, "packaged-broker-logs")]);
        var pipe = BrokerProcess.NewPipeName();
        using var process = BrokerProcess.Start(exe, pipe, log, elevated: true);
        try
        {
            using var rpc = new BrokerRpc(pipe);
            PingInfo? ping = null;
            for (var attempt = 0; attempt < 40 && ping is null; attempt++)
            {
                try { ping = rpc.PingAsync(TimeSpan.FromSeconds(2)).GetAwaiter().GetResult(); }
                catch (Exception ex) when (ex is HttpRequestException or IOException or OperationCanceledException or GrpcStatusException or InvalidDataException or TimeoutException) { Thread.Sleep(250); }
            }
            Check(ping is not null, "the packaged Broker answers Ping");
            if (ping is null) return;
            var resolved = BrokerCapabilities.Resolve(ping, exe);
            Console.WriteLine($"packaged Broker {exe}: {ping}; resolved: {resolved}");
            Same(CapabilitySource.Ping, resolved.Source, "the packaged Broker lists its features itself");
            Check(resolved.Has("endfield") && resolved.Has("screen-args") && resolved.Has("touch-ui-switch"), "the packaged Broker reports endfield, screen-args and touch-ui-switch");
            // its capabilities.json says the same, so a launcher that cannot read the Ping list gets the same answer
            var fromManifest = BrokerCapabilities.Resolve(new PingInfo(1, "x", []), exe);
            Same(CapabilitySource.Manifest, fromManifest.Source, "the package ships capabilities.json");
            Check(fromManifest.Names.SetEquals(resolved.Names), "capabilities.json equals the Ping list");
        }
        finally
        {
            try { process.Kill(entireProcessTree: true); } catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception) { }
        }
    }
}
