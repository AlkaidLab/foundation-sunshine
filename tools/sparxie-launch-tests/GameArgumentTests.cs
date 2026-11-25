namespace Sparxie.Launch;

internal static partial class Tests
{
    private static void ExtraGameArguments(string scratch)
    {
        var config = Path.Combine(scratch, "launch-options.json");
        File.WriteAllText(config, """
            {"schemaVersion":1,"profiles":[
              {"id":"zzz-cn-1","game":"zenlessZoneZero","executablePath":"C:\\ZZZ\\ZenlessZoneZero.exe",
               "launch":{"extraArguments":"\"-use-d3d12\" -force-d3d11 --label \"keep -force-d3d11\""}},
              {"id":"old","game":"zenlessZoneZero","executablePath":"C:\\ZZZ\\ZenlessZoneZero.exe"}
            ]}
            """);
        var profile = SparxieConfig.Load(config, "zzz-cn-1");
        const string custom = "--label \"keep -force-d3d11\"";
        Same($"\"-use-d3d12\" -force-d3d11 {custom}", profile.ExtraArguments, "the Sparxie profile carries its native launch arguments");
        Same("", SparxieConfig.Load(config, "old").ExtraArguments, "a legacy profile needs no launch block");

        var baseOptions = new[] { "--sparxie", @"C:\Sparxie\Sparxie.exe", "--profile", "zzz-cn-1" };
        var options = LaunchOptions.Parse([.. baseOptions, "--screen-width", "1920", "--screen-height", "1080", "--window-mode", "fullscreen",
            "--zzz-dx12=on", "--game-args", "-use-d3d12 -screen-quality 2"], out _)!;
        var warnings = new List<string>();
        var merged = GameArguments.Resolve(profile, options, warnings.Add);
        Same($"-use-d3d12 -screen-quality 2 {custom}", merged, "the app DX12 selection wins, exactly once, and other arguments keep their quoting");
        Same($"-screen-quality 2 {custom}", GameArguments.Resolve(profile, options with { ZzzDx12 = false }, warnings.Add), "an explicit DX11 selection removes conflicting renderer flags");
        Same(profile.ExtraArguments, GameArguments.Resolve(profile, options with { ZzzDx12 = null, ExtraArguments = "" }, warnings.Add), "without an app choice the profile supplies the arguments");
        Same("-use-d3d12", GameArguments.Resolve(profile with { ExtraArguments = "" }, options with { ExtraArguments = "" }, warnings.Add), "the app can choose DX12 when the profile has no launch settings");
        Same("", GameArguments.Resolve(profile with { Game = "starRail", ExtraArguments = "" }, options with { ExtraArguments = "" }, warnings.Add), "the Zenless renderer switch is only added to Zenless");
        Check(warnings.Count == 0, "valid launch arguments need no warning");
        Same("-use-d3d12", GameArguments.Resolve(profile with { ExtraArguments = "bad\nvalue" }, options with { ExtraArguments = "" }, warnings.Add), "invalid profile text does not drop the app renderer switch");
        Same("-use-d3d12", GameArguments.Resolve(profile with { ExtraArguments = new string('a', 512) }, options with { ExtraArguments = "" }, warnings.Add), "the combined arguments fit the Broker's limit");
        Check(warnings.Count == 2, "an ignored value is logged");

        var requested = profile with { ExtraArguments = merged, Screen = options.Screen };
        List<ProtoField> Snapshot(BrokerCapabilities capabilities, Profile? p = null) =>
            Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(p ?? requested, "r", capabilities)).Single(f => f.Number == 3).Bytes);
        var stock = new BrokerCapabilities(new HashSet<string>(), CapabilitySource.InstalledFiles, HasLaunchArguments: true);
        var sky = new BrokerCapabilities(new HashSet<string> { "screen-args" }, CapabilitySource.Ping, HasLaunchArguments: true);
        var old = stock with { HasLaunchArguments = false };
        var stockFields = Snapshot(stock);
        Same(merged, Proto.String(Proto.Parse(stockFields.Single(f => f.Number == 6).Bytes), 4), "LaunchSettings carries the original argument text in extra_arguments");
        Check(!stockFields.Any(f => f.Number == 7), "stock has no ScreenArgs field");
        var skyFields = Snapshot(sky);
        Same(merged, Proto.String(Proto.Parse(skyFields.Single(f => f.Number == 6).Bytes), 4), "the fork also sends extra_arguments in LaunchSettings");
        Same("fullscreen", Proto.String(Proto.Parse(skyFields.Single(f => f.Number == 7).Bytes), 3), "the fork sends the screen plan in its own message");
        Check(!Snapshot(old).Any(f => f.Number is 6 or 7), "old Brokers receive neither unsupported settings field");
        Same("{\"enabled\":true}", Proto.String(Snapshot(sky, requested with { Game = GameIds.EndfieldPlugin, PluginSettings = "{\"enabled\":true}" }), 7), "plugin settings keep their field");
        Check(LaunchNotice.Messages(LaunchNotice.KindArgumentsOmitted).Zh.Contains("附加启动参数"), "an old Broker's launch can report the omission to the UI");
    }
}
