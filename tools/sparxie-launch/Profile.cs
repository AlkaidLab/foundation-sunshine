using System.Text.Json;

namespace Sparxie.Launch;

internal sealed record HoyoSettings(bool FpsUnlock, int TargetFps, bool BackgroundLimit, int BackgroundFps,
    string Priority, bool FollowPreset, int Preset30, int Preset60, bool ScaleOverride, int Scale, bool TouchUi = true,
    string EndfieldInput = "", bool EndfieldAutoResolution = true, int EndfieldWidth = 0, int EndfieldHeight = 0, int EndfieldRefresh = 0)
{
    public static HoyoSettings FromJson(JsonElement h) => new(
        GetBool(h, "fpsUnlockEnabled", true), GetInt(h, "targetFps", 120), GetBool(h, "backgroundFpsLimitEnabled", true),
        GetInt(h, "backgroundFps", 10),
        h.TryGetProperty("processPriority", out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "normal" : "normal",
        GetBool(h, "genshinFollowInGamePreset", false), GetInt(h, "genshinPreset30Fps", 60), GetInt(h, "genshinPreset60Fps", 1000),
        GetBool(h, "genshinTouchUiScaleOverrideEnabled", false), GetInt(h, "genshinTouchUiScalePercent", 400),
        GetBool(h, "touchUiEnabled", true),
        h.TryGetProperty("endfieldInputMode", out var m) && m.ValueKind == JsonValueKind.String ? m.GetString() ?? "" : "",
        GetBool(h, "endfieldAutoResolution", true), GetInt(h, "endfieldWidth", 0), GetInt(h, "endfieldHeight", 0), GetInt(h, "endfieldRefreshRate", 0));

    private static bool GetBool(JsonElement e, string key, bool fallback) =>
        e.TryGetProperty(key, out var v) ? v.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => fallback } : fallback;

    private static int GetInt(JsonElement e, string key, int fallback) =>
        e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n) ? n : fallback;

    public string Describe() =>
        $"fpsUnlock={FpsUnlock} targetFps={TargetFps} bgLimit={BackgroundLimit} bgFps={BackgroundFps} priority={Priority} followPreset={FollowPreset} " +
        $"preset30={Preset30} preset60={Preset60} scaleOverride={ScaleOverride} scale={Scale} touchUi={TouchUi}" +
        (EndfieldInput.Length > 0 ? $" endfieldInput={EndfieldInput} endfieldAutoResolution={EndfieldAutoResolution} endfieldSize={EndfieldWidth}x{EndfieldHeight} endfieldRefresh={EndfieldRefresh}" : "");
}

/// <summary>
/// An Endfield session is told the stream's size and refresh rate: the Broker protocol has the fields (endfield_input_mode 12, endfield_auto_resolution 13, endfield_width 14,
/// endfield_height 15, endfield_refresh_rate 16). The input is the person's choice, touch or pc (keyboard, mouse and gamepad); there is no automatic input any more (the host migrates
/// an app that still says auto, and a stored "auto" that reaches the launcher is touch). The size and rate come from the variables Sunshine gives the application:
/// SUNSHINE_CLIENT_WIDTH/HEIGHT and SUNSHINE_CLIENT_FPS.
/// </summary>
internal static class EndfieldSession
{
    public static Profile Resolve(Profile profile, Func<string, string?> env)
    {
        if (profile.Game != "endfield" || profile.Hoyo is not { } h) return profile;
        var input = h.EndfieldInput == "pc" ? "pc" : "touch";
        var auto = h.EndfieldAutoResolution;
        int width = h.EndfieldWidth, height = h.EndfieldHeight;
        if (auto && Positive(env("SUNSHINE_CLIENT_WIDTH")) is { } w && Positive(env("SUNSHINE_CLIENT_HEIGHT")) is { } hh)
        {
            // the person left the resolution to the stream: the stream's size is sent
            auto = false; width = w; height = hh;
        }
        var refresh = h.EndfieldRefresh > 0 ? h.EndfieldRefresh : Positive(env("SUNSHINE_CLIENT_FPS")) ?? 0;
        return profile with { Hoyo = h with { EndfieldInput = input, EndfieldAutoResolution = auto, EndfieldWidth = width, EndfieldHeight = height, EndfieldRefresh = refresh } };
    }

    private static int? Positive(string? text) => int.TryParse(text, out var n) && n > 0 ? n : null;
}

internal sealed record Profile(string Id, string DisplayName, string Game, string ExecutablePath, HoyoSettings? Hoyo, ScreenArgs? Screen = null)
{
    public static Profile FromJson(JsonElement value)
    {
        string Str(string key) => value.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
        var game = Str("game");
        if (game.Length == 0) throw new InvalidDataException("the profile has no game");
        HoyoSettings? hoyo = null;
        if (value.TryGetProperty("hoyo", out var h) && h.ValueKind == JsonValueKind.Object) hoyo = HoyoSettings.FromJson(h);
        return new Profile(Str("id"), Str("displayName"), game, Str("executablePath"), hoyo);
    }
}

internal static class SparxieConfig
{
    public const string DataDirEnv = "SPARXIE_DATA_DIR";

    /// <summary>
    /// Where config.json may be, in the host's own order (docs/sparxie-api.md): SPARXIE_DATA_DIR alone when set, else the folder above app-*
    /// (the portable root, beside Sparxie.exe), else %LOCALAPPDATA%\Sparxie, which Sparxie uses when the portable root is not writable.
    /// </summary>
    public static IReadOnlyList<string> DataDirectories(string sparxiePath, string? envOverride, string? localAppData)
    {
        if (!string.IsNullOrWhiteSpace(envOverride)) return [envOverride];
        var list = new List<string>();
        var dir = Path.GetDirectoryName(sparxiePath) ?? ".";
        var name = Path.GetFileName(dir.TrimEnd('\\', '/'));
        if (name.StartsWith("app-", StringComparison.OrdinalIgnoreCase))
            dir = Path.GetDirectoryName(dir.TrimEnd('\\', '/')) ?? dir;
        list.Add(dir);
        if (!string.IsNullOrWhiteSpace(localAppData)) list.Add(Path.Combine(localAppData, "Sparxie"));
        return list;
    }

    /// <summary>The first data directory that has a config.json; else the first one (so the error names a real place).</summary>
    public static string PickDataDirectory(IReadOnlyList<string> candidates) =>
        candidates.FirstOrDefault(d => File.Exists(Path.Combine(d, "config.json"))) ?? candidates[0];

    /// <summary>Loads one profile; every failure is an exception whose message is one readable line.</summary>
    public static Profile Load(string configPath, string profileId)
    {
        if (!File.Exists(configPath)) throw new FileNotFoundException($"Sparxie config.json was not found: {configPath}");
        JsonDocument document;
        try { document = JsonDocument.Parse(File.ReadAllText(configPath)); }
        catch (JsonException ex) { throw new InvalidDataException($"Sparxie config.json is not valid JSON ({ex.Message})"); }
        using (document)
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("profiles", out var profiles) || profiles.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("Sparxie config.json has no profiles list");
            foreach (var item in profiles.EnumerateArray())
            {
                if (item.ValueKind == JsonValueKind.Object && item.TryGetProperty("id", out var id) && id.GetString() == profileId)
                    return Profile.FromJson(item);
            }
            throw new InvalidDataException($"Sparxie profile not found: {profileId}{MovedAsideHint(configPath, profileId)}");
        }
    }

    /// <summary>
    /// Sparxie keeps the previous config.json beside it when it changes the file: config.invalid-yyyyMMddHHmmss-xxxxxxxx.json when it could not read the file at all (a new config.json was started;
    /// 2026-11-12: every profile vanished that way and each launch said only "profile not found") and config.repaired-*.json when it repaired or left out profiles.
    /// When such a file is beside config.json this names the newest and says whether it holds the profile.
    /// </summary>
    public static string MovedAsideHint(string configPath, string profileId)
    {
        try
        {
            var dir = Path.GetDirectoryName(Path.GetFullPath(configPath));
            if (dir is null || !Directory.Exists(dir)) return "";
            var newest = Directory.GetFiles(dir, "config.invalid-*.json").Concat(Directory.GetFiles(dir, "config.repaired-*.json"))
                .OrderByDescending(f => File.GetLastWriteTimeUtc(f)).ThenByDescending(f => Path.GetFileName(f), StringComparer.Ordinal).FirstOrDefault();
            if (newest is null) return "";
            var name = Path.GetFileName(newest);
            var holds = File.ReadAllText(newest).Contains($"\"{profileId}\"", StringComparison.Ordinal);
            return holds
                ? $". Sparxie set aside the previous config as {name} when it changed config.json; that file still has this profile. Close Sparxie and restore it from there ({dir})"
                : $". Sparxie set aside the previous config as {name} when it changed config.json (it does not have this profile either) ({dir})";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return "";
        }
    }
}
