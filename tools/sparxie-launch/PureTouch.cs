using System.Security.Cryptography;

namespace Sparxie.Launch;

/// <summary>
/// Sparxie's "touch only" mode (frame-rate unlock off) fails in HoyoTouchCore builds up to preview.9: with no frame-rate pointer, inject_patch returns 0 and the session
/// ends Failed with Hoyo error 6 (Star Rail at once, Genshin after its scan). Until a fixed runtime is installed, a Hoyo session is sent with the unlock on and the game's own
/// default cap, which looks the same in the game and keeps the touch layer.
/// </summary>
internal static class PureTouch
{
    /// <summary>The cap the game itself starts with, used as the unlock target when the person turned the unlock off.</summary>
    public const int GameOwnCap = 60;

    /// <summary>SHA-256 of HoyoTouchCore.dll builds known to fail touch-only sessions: preview.9 (222208 bytes).</summary>
    public static readonly HashSet<string> KnownBrokenRuntimes = new(StringComparer.OrdinalIgnoreCase)
    {
        "1A0897A111CF35AB07EB34A2B9A2BE8D072221FA2F10A38923308EF89A0FB0A4",
    };

    public static bool IsTouchOnly(Profile profile) => profile.Game is "genshin" or "starRail" && profile.Hoyo is { FpsUnlock: false };

    public static Profile WithUnlock(Profile profile) =>
        IsTouchOnly(profile) ? profile with { Hoyo = profile.Hoyo! with { FpsUnlock = true, TargetFps = GameOwnCap } } : profile;

    /// <summary>The hash of HoyoTouchCore.dll beside the Broker, or null when it cannot be read.</summary>
    public static string? RuntimeHash(string brokerPath)
    {
        try
        {
            var dll = Path.Combine(Path.GetDirectoryName(brokerPath) ?? ".", "HoyoTouchCore.dll");
            if (!File.Exists(dll)) return null;
            using var stream = File.OpenRead(dll);
            return Convert.ToHexString(SHA256.HashData(stream));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
    }
}
