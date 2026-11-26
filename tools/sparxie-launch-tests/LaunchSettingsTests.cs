namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- official Sparxie 0.2.0: field 6 of the profile snapshot is LaunchSettings (the profile's "launch" block), not ScreenArgs ----
    private static void LaunchSettingsSession(string scratch)
    {
        var config = Path.Combine(scratch, "launch-config.json");
        File.WriteAllText(config, """
            { "schemaVersion": 1, "profiles": [
              { "id": "own", "displayName": "星铁", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe",
                "launch": { "width": 2560, "height": 1440, "fullscreen": false, "borderless": true, "extraArguments": " -force-d3d12 " } },
              { "id": "plain", "displayName": "星铁", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe" },
              { "id": "half", "displayName": "星铁", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe", "launch": { "width": 2560, "fullscreen": null, "borderless": false } } ] }
            """);
        var own = SparxieConfig.Load(config, "own");
        Same(new LaunchSettings(2560, 1440, false, true, "-force-d3d12"), own.Launch, "the profile's launch block is read");
        Check(SparxieConfig.Load(config, "plain").Launch is null && SparxieConfig.Load(config, "half").Launch is null, "no block, or a block that holds nothing usable: no launch settings");

        // the capability: from the installed contracts, and never beside screen-args
        BrokerCapabilities Installed(string name, params string[] markers)
        {
            var dir = Directory.CreateDirectory(Path.Combine(scratch, "launch-" + name)).FullName;
            File.WriteAllText(Path.Combine(dir, "Sparxie.Contracts.dll"), string.Join('\0', markers));
            return BrokerCapabilities.Resolve(null, Path.Combine(dir, "Sparxie.Broker.exe"));
        }
        var official = Installed("official", "LaunchSettings", "ExtraArguments", "GameIds", "SettingsJson");
        var screenBuild = Installed("screen", "ScreenArgs", "ExtraArguments");
        var bare = Installed("bare", "GameIds");
        Check(official.Has(PingInfo.LaunchSettingsCapability) && !official.Has(PingInfo.ScreenArgsCapability), "official Sparxie takes launch settings");
        Check(screenBuild.Has(PingInfo.ScreenArgsCapability) && !screenBuild.Has(PingInfo.LaunchSettingsCapability), "a screen-args build is not sent launch settings");
        var officialBroker = Path.Combine(scratch, "launch-official", "Sparxie.Broker.exe");
        var listing = BrokerCapabilities.Resolve(new PingInfo(1, "0.3.0", [PingInfo.Endfield]), officialBroker);
        Check(listing.Has(PingInfo.Endfield) && listing.Has(PingInfo.LaunchSettingsCapability) && listing.Source == CapabilitySource.Ping,
            "an official Broker that lists other features still takes launch settings");
        File.WriteAllText(Path.Combine(scratch, "launch-official", BrokerCapabilities.ManifestFileName), """{ "schema": 1, "capabilities": [ "touch-ui-switch" ] }""");
        Check(BrokerCapabilities.Resolve(null, officialBroker).Has(PingInfo.LaunchSettingsCapability), "and so does one with a capabilities.json");
        var screenListed = BrokerCapabilities.Resolve(new PingInfo(1, "x", [PingInfo.ScreenArgsCapability]), officialBroker);
        Check(!screenListed.Has(PingInfo.LaunchSettingsCapability), "a Broker that lists screen-args is never sent launch settings");

        Profile Supported(Profile p, BrokerCapabilities caps, List<string>? warnings = null)
        {
            Same(null, Launcher.CheckSupport(ref p, caps, w => warnings?.Add(w)), "launch settings never stop a session");
            return p;
        }
        List<ProtoField>? Field6(Profile p)
        {
            var six = Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(p, "r")).First(x => x.Number == 3).Bytes).Where(x => x.Number == 6).ToList();
            Check(six.Count <= 1, "field 6 is sent at most once");
            return six.Count == 0 ? null : Proto.Parse(six[0].Bytes);
        }

        var asStored = Field6(Supported(own, official));
        Check(asStored is not null && Proto.Int(asStored, 1) == 2560 && Proto.Int(asStored, 2) == 1440 && Proto.Int(asStored, 3) == 2 && Proto.String(asStored, 4) == "-force-d3d12" && Proto.Bool(asStored, 5),
            "without a screen plan the profile's own launch settings go on the wire (width 1, height 2, fullscreen 3, extra 4, borderless 5)");
        Check(Field6(Supported(SparxieConfig.Load(config, "plain"), official)) is null, "nothing to say: no field 6");

        var follow = Supported(own with { Screen = new ScreenArgs(2388, 1668, ScreenArgs.Fullscreen) }, official);
        Check(follow.Screen is null, "official Sparxie is never sent ScreenArgs");
        Same(new LaunchSettings(2388, 1668, true, false, "-force-d3d12"), follow.Launch, "the stream's size and window win, the extra arguments stay");
        Same(1, Proto.Int(Field6(follow)!, 3), "full screen is 1");
        Same(new LaunchSettings(1280, 720, false, true), LaunchSettings.Merge(null, new ScreenArgs(1280, 720, ScreenArgs.Borderless)), "a borderless plan is a window with -popupwindow");
        Same(new LaunchSettings(1280, 720, false, false), LaunchSettings.Merge(null, new ScreenArgs(1280, 720, ScreenArgs.Windowed)), "a window");
        Check(LaunchSettings.Merge(null, new ScreenArgs(0, 0, ScreenArgs.Fullscreen)) is null, "a mode without a size is not sent: Sparxie would write 1920x1080 for it");
        Same(new LaunchSettings(2560, 1440, true, false, "-force-d3d12"), LaunchSettings.Merge(own.Launch, new ScreenArgs(0, 0, ScreenArgs.Fullscreen)), "a mode over the profile's size");
        Check(LaunchSettings.Merge(null, new ScreenArgs(400, 320, ScreenArgs.Fullscreen)) is null, "a size the Broker would refuse is not sent");
        Same(new LaunchSettings(0, 0, null, false, "-force-d3d12"), LaunchSettings.Merge(own.Launch, new ScreenArgs(400, 320, ScreenArgs.Fullscreen)),
            "a refused size does not bring the profile's size back over the host's write");

        var onScreenBuild = Supported(own with { Screen = new ScreenArgs(2388, 1668, ScreenArgs.Fullscreen) }, screenBuild);
        Check(onScreenBuild.Launch is null && onScreenBuild.Screen is not null, "a screen-args build gets ScreenArgs only");
        Same("fullscreen", Proto.String(Field6(onScreenBuild)!, 3), "field 6 there is ScreenArgs");
        Check(Field6(Supported(own, screenBuild)) is null, "and nothing when the host asked for nothing");
        var warned = new List<string>();
        Check(Field6(Supported(own with { Screen = new ScreenArgs(1280, 720, ScreenArgs.Windowed) }, bare, warned)) is null && warned.Count == 1, "a build with neither gets no field 6 and one warning");

        // a start without Sparxie keeps the person's arguments
        Same("-popupwindow -force-d3d12", LaunchSettings.DirectArguments(own.Launch, null), "direct start: the profile's borderless window and extra arguments");
        Same("-screen-width 1280 -screen-height 720 -screen-fullscreen 0 -force-d3d12", LaunchSettings.DirectArguments(own.Launch, new ScreenArgs(1280, 720, ScreenArgs.Windowed)),
            "direct start: the host's screen plan, then the extra arguments");
        Same("-screen-width 1280 -screen-height 720 -popupwindow -force-d3d12", LaunchSettings.DirectArguments(own.Launch, new ScreenArgs(1280, 720, "")),
            "direct start: a size-only screen plan keeps the profile's borderless -popupwindow");
        Same("-screen-width 1280 -screen-height 720 -popupwindow -x", LaunchSettings.DirectArguments(new LaunchSettings(Borderless: true, ExtraArguments: "-popupwindow -x"), new ScreenArgs(1280, 720, "")),
            "direct start: -popupwindow is not repeated when ExtraArguments already carries it");
        Same("-popupwindow -x", new LaunchSettings(Borderless: true, ExtraArguments: "-popupwindow -x").Arguments(), "-popupwindow is not repeated");
        Same("", LaunchSettings.DirectArguments(null, null), "direct start: nothing asked");
    }
}
