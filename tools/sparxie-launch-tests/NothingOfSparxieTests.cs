namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- a profile that turns both the touch interface and the unlock off uses nothing of Sparxie: no session, and the unlock is never turned on for it ----
    private static void NothingOfSparxieRules()
    {
        static Profile Of(string game, bool? unlock, bool disableTouchUi, string? plugin = null) =>
            new("p", "P", game, @"C:\Games\game.exe", unlock is { } on ? new HoyoSettings(on, 120, true, 10, "normal", false, 60, 1000, false, 400) : null,
                PluginSettings: plugin, DisableTouchUi: disableTouchUi);

        foreach (var game in new[] { "genshin", "starRail" })
        {
            var keyboard = Of(game, unlock: false, disableTouchUi: true);
            Check(keyboard.UsesNothingOfSparxie, game + ": keyboard without the unlock");
            Check(!PureTouch.IsTouchOnly(keyboard), game + ": keyboard without the unlock is not touch only");
            Check(PureTouch.WithUnlock(keyboard).Hoyo is { FpsUnlock: false }, game + ": a retry never turns the unlock on for it");
            Check(!Of(game, unlock: true, disableTouchUi: true).UsesNothingOfSparxie, game + ": keyboard with the unlock goes through Sparxie");
            Check(!Of(game, unlock: false, disableTouchUi: false).UsesNothingOfSparxie && PureTouch.IsTouchOnly(Of(game, unlock: false, disableTouchUi: false)),
                game + ": the touch interface alone goes through Sparxie");
        }
        Check(Of("zenlessZoneZero", unlock: null, disableTouchUi: true).UsesNothingOfSparxie, "Zenless Zone Zero has no unlock: keyboard uses nothing");
        Check(!Of("zenlessZoneZero", unlock: null, disableTouchUi: false).UsesNothingOfSparxie, "Zenless Zone Zero with touch goes through Sparxie");
        Check(Of(GameIds.EndfieldPlugin, unlock: null, disableTouchUi: true, plugin: """{"fpsUnlock":false}""").UsesNothingOfSparxie, "Endfield keyboard without the unlock");
        Check(!Of(GameIds.EndfieldPlugin, unlock: null, disableTouchUi: true, plugin: """{"fpsUnlock":true}""").UsesNothingOfSparxie, "Endfield keyboard with the unlock");
        Check(!Of(GameIds.EndfieldPlugin, unlock: null, disableTouchUi: true).UsesNothingOfSparxie, "a plugin without settings runs on its default, which unlocks");
    }
}
