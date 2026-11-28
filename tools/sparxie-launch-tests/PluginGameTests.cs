using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- the second Sparxie generation: Endfield is the plugin ext.skyhua0224.endfield, and its options travel as settings_json (field 7) ----
    private static void PluginGames(string scratch)
    {
        // the id syntax is Sparxie's own
        foreach (var good in new[] { "ext.skyhua0224.endfield", "ext.ab.cd", "ext.a1-b2.c3-d4" })
            Check(GameIds.IsPlugin(good), $"{good} is a plugin id");
        foreach (var bad in new[] { "endfield", "genshin", "ext.a.endfield", "ext.vendor.", "Ext.vendor.game", "ext.vendor.game.extra", "ext.vendor.Game", "xext.vendor.game", "ext.vendor.game\n", "" })
            Check(!GameIds.IsPlugin(bad), $"'{bad.Replace("\n", "\\n")}' is not a plugin id");
        Check(GameIds.IsEndfield("endfield") && GameIds.IsEndfield(GameIds.EndfieldPlugin) && !GameIds.IsEndfield("ext.other.endfield") && !GameIds.IsEndfield("genshin"), "Endfield is its two ids");

        // the profile as the host writes it for the second generation
        var config = Path.Combine(scratch, "plugin-config.json");
        File.WriteAllText(config, """
            { "schemaVersion": 1, "selectedProfileId": "endfield-cn-1", "profiles": [
              { "id": "endfield-cn-1", "displayName": "终末地", "game": "ext.skyhua0224.endfield", "executablePath": "C:\\E\\Endfield.exe",
                "settings": { "inputMode": "pc", "autoResolution": true, "width": 1920, "height": 1080, "refreshRate": 60, "fpsUnlock": true, "targetFps": 120, "uiScalePercent": 150 } },
              { "id": "bare", "displayName": "无设置", "game": "ext.skyhua0224.endfield", "executablePath": "C:\\E\\Endfield.exe" },
              { "id": "star", "displayName": "星铁", "game": "starRail", "executablePath": "C:\\S\\StarRail.exe", "hoyo": { "fpsUnlockEnabled": true, "targetFps": 120 } } ] }
            """);
        var plugin = SparxieConfig.Load(config, "endfield-cn-1");
        Same(GameIds.EndfieldPlugin, plugin.Game, "the plugin game id is read");
        Check(plugin.Hoyo is null, "a plugin profile has no hoyo block");
        Check(plugin.PluginSettings is not null && JsonNode.Parse(plugin.PluginSettings)!["uiScalePercent"]!.GetValue<int>() == 150, "the plugin's own settings are kept whole, the ones the host does not manage too");
        Check(SparxieConfig.Load(config, "bare").PluginSettings is null, "a plugin profile without settings has none");
        Check(SparxieConfig.Load(config, "star").PluginSettings is null, "a built-in game carries no plugin settings");

        // the request: settings_json = 7 in the profile snapshot, no hoyo (5)
        List<ProtoField> Snapshot(Profile p) => Proto.Parse(Proto.Parse(BrokerRpc.StartSessionPayload(p, "r")).First(x => x.Number == 3).Bytes);
        var wire = Snapshot(plugin);
        Same(GameIds.EndfieldPlugin, Proto.String(wire, 3), "the game id on the wire");
        Check(!wire.Any(x => x.Number == 5), "a plugin session has no hoyo settings");
        Same(plugin.PluginSettings, Proto.String(wire, 7), "settings_json is the profile's settings as they are");
        Check(!Snapshot(SparxieConfig.Load(config, "bare")).Any(x => x.Number == 7), "no settings, no field: the plugin runs on its defaults");
        Check(!Snapshot(SparxieConfig.Load(config, "star")).Any(x => x.Number == 7), "a built-in game never sends settings_json");
        Check(!Snapshot(new Profile("x", "x", "starRail", @"C:\S\StarRail.exe", null, null, "{\"a\":1}")).Any(x => x.Number == 7), "settings on a built-in game id are not sent");

        // the stream's size reaches a profile that follows the main display; the person's choices win
        static string? Stream(string name) => name switch { "SUNSHINE_CLIENT_WIDTH" => "2388", "SUNSHINE_CLIENT_HEIGHT" => "1668", "SUNSHINE_CLIENT_FPS" => "120", _ => null };
        JsonObject Resolved(Profile p, Func<string, string?> env) => JsonNode.Parse(EndfieldSession.Resolve(p, env).PluginSettings!)!.AsObject();
        var auto = Resolved(plugin, Stream);
        Same(false, auto["autoResolution"]!.GetValue<bool>(), "the stream's size replaces the automatic resolution");
        Same(2388, auto["width"]!.GetValue<int>(), "stream width");
        Same(1668, auto["height"]!.GetValue<int>(), "stream height");
        Same(60, auto["refreshRate"]!.GetValue<int>(), "the stored refresh rate (the host's value of the streamed display) is kept");
        Same("pc", auto["inputMode"]!.GetValue<string>(), "the input is the person's");
        Same(150, auto["uiScalePercent"]!.GetValue<int>(), "the settings the host does not manage are untouched");
        var noRate = Resolved(plugin with { PluginSettings = """{ "autoResolution": true, "refreshRate": 0 }""" }, Stream);
        Same(120, noRate["refreshRate"]!.GetValue<int>(), "no stored rate: the stream's");
        var untouched = EndfieldSession.Resolve(plugin, _ => null);
        Same(plugin.PluginSettings, untouched.PluginSettings, "no stream size: the settings stay as they are");
        var fixedSize = Resolved(plugin with { PluginSettings = """{ "autoResolution": false, "width": 1920, "height": 1080, "refreshRate": 165 }""" }, Stream);
        Check(fixedSize["width"]!.GetValue<int>() == 1920 && fixedSize["height"]!.GetValue<int>() == 1080 && fixedSize["refreshRate"]!.GetValue<int>() == 165 && !fixedSize["autoResolution"]!.GetValue<bool>(), "a fixed size stays");
        var missingFlag = Resolved(plugin with { PluginSettings = """{ "inputMode": "touch" }""" }, Stream);
        Same(2388, missingFlag["width"]!.GetValue<int>(), "autoResolution is on by default (the plugin's schema)");
        var huge = Resolved(plugin, name => name switch { "SUNSHINE_CLIENT_WIDTH" => "99999", "SUNSHINE_CLIENT_HEIGHT" => "10", "SUNSHINE_CLIENT_FPS" => "5000", _ => null });
        Check(huge["width"]!.GetValue<int>() == 7680 && huge["height"]!.GetValue<int>() == 240, "a size outside the plugin's range is clamped");
        var noSettings = new Profile("n", "n", GameIds.EndfieldPlugin, @"C:\E\Endfield.exe", null);
        Check(EndfieldSession.Resolve(noSettings, Stream).PluginSettings is null, "a profile without settings gets none made up");
        var other = new Profile("o", "o", "ext.other.game", @"C:\O\Other.exe", null, null, """{ "autoResolution": true }""");
        Same(other.PluginSettings, EndfieldSession.Resolve(other, Stream).PluginSettings, "another plugin's settings are never touched");

        // the support check: the second generation lists no capabilities, so the plugin profile is not held back by "no endfield capability"
        var none = new BrokerCapabilities(new HashSet<string>(), CapabilitySource.InstalledFiles);
        var forPlugin = plugin;
        Same(null, Launcher.CheckSupport(ref forPlugin, none, _ => { }), "a plugin Endfield session starts on a Broker that lists nothing");
        var legacy = new Profile("l", "l", "endfield", @"C:\E\Endfield.exe", null);
        Check(Launcher.CheckSupport(ref legacy, none, _ => { }) is not null, "the first generation's Endfield still needs its capability");

        // when Sparxie cannot take the session, the direct start of Endfield goes under the decoy parent in either spelling
        Check(DirectLaunch.UsesDecoyParent(plugin) && DirectLaunch.UsesDecoyParent(legacy) && !DirectLaunch.UsesDecoyParent(SparxieConfig.Load(config, "star")), "Endfield is started under the decoy parent whatever its spelling");

        // the 2026-11-23 wipe: an old profile that says "endfield" in a build that reads the plugin's id is for the host to repair; the launcher still reads it as the old title
        var oldConfig = Path.Combine(scratch, "old-config.json");
        File.WriteAllText(oldConfig, """{ "schemaVersion": 1, "profiles": [ { "id": "e", "displayName": "e", "game": "endfield", "executablePath": "C:\\E\\Endfield.exe", "hoyo": { "endfieldInputMode": "pc", "targetFps": 120 } } ] }""");
        var old = SparxieConfig.Load(oldConfig, "e");
        Check(old.Game == "endfield" && old.Hoyo is { EndfieldInput: "pc" } && old.PluginSettings is null, "the first generation's profile is read as before");
    }
}
