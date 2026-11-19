using System.Text;
using System.Text.Json;

namespace Sparxie.Launch;

/// <summary>
/// A one-line note for the Web UI and the Panel, written next to the host's Sparxie data (config\sparxie\launch-notice.json). The host reports it with the Sparxie status.
/// A clean launch removes it, so it always describes the last launch.
/// </summary>
internal static class LaunchNotice
{
    public const string FileName = "launch-notice.json";
    public const string DirEnv = "SPARXIE_LAUNCH_NOTICE_DIR";

    public const string KindDirect = "direct";           // Sparxie could not start the session, the game was started directly
    public const string KindRetried = "retried";         // the first session failed, the automatic retry worked
    public const string KindUnlock = "unlock_forced";    // touch only (unlock off) is not supported by this Sparxie runtime: the game started with the unlock at the game's own cap

    public static (string Zh, string En) Messages(string kind) => kind switch
    {
        KindDirect => ("Sparxie 未能接管本次启动，已直接启动游戏", "Sparxie could not take over this launch; the game was started directly"),
        KindRetried => ("Sparxie 首次启动失败，已自动重试并成功", "Sparxie failed the first time; the automatic retry worked"),
        KindUnlock => ($"当前 Sparxie 运行时不支持纯触屏（关闭帧率解锁），本次已按 {PureTouch.GameOwnCap} FPS 解锁启动", $"This Sparxie runtime cannot run touch only (frame-rate unlock off); the game started with the unlock at {PureTouch.GameOwnCap} FPS"),
        _ => ("", ""),
    };

    public static IEnumerable<string> CandidateDirectories(string exeDirectory, Func<string, string?> env)
    {
        var over = env(DirEnv);
        if (!string.IsNullOrWhiteSpace(over)) yield return over;
        var trimmed = exeDirectory.TrimEnd('\\', '/');
        var install = Path.GetDirectoryName(trimmed);
        if (!string.IsNullOrEmpty(install) && string.Equals(Path.GetFileName(trimmed), "tools", StringComparison.OrdinalIgnoreCase))
            yield return Path.Combine(install, "config", "sparxie");
        var local = env("LOCALAPPDATA");
        if (!string.IsNullOrWhiteSpace(local)) yield return Path.Combine(local, "Sunshine");
    }

    public static string Build(string kind, Profile profile, string reason, string? logPath, DateTime nowUtc)
    {
        var (zh, en) = Messages(kind);
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = false, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            w.WriteStartObject();
            w.WriteNumber("version", 1);
            w.WriteString("id", Guid.NewGuid().ToString("N"));
            w.WriteString("time", nowUtc.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture));
            w.WriteString("kind", kind);
            w.WriteString("profile", profile.Id);
            w.WriteString("game", profile.Game);
            w.WriteString("message", zh);
            w.WriteString("messageEn", en);
            w.WriteString("reason", reason);
            w.WriteString("log", logPath ?? "");
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    /// <summary>Writes the note into the first folder that works; returns where, or null.</summary>
    public static string? Write(IEnumerable<string> directories, string json)
    {
        foreach (var dir in directories)
        {
            try
            {
                Directory.CreateDirectory(dir);
                var path = Path.Combine(dir, FileName);
                var temp = path + ".tmp";
                File.WriteAllText(temp, json, new UTF8Encoding(false));
                File.Move(temp, path, true);
                return path;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException or NotSupportedException or ArgumentException) { }
        }
        return null;
    }

    public static void Clear(IEnumerable<string> directories)
    {
        foreach (var dir in directories)
        {
            try { var path = Path.Combine(dir, FileName); if (File.Exists(path)) File.Delete(path); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
    }
}
