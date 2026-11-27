namespace Sparxie.Launch;

internal static partial class Tests
{
    // ---- a config shared with an external Sparxie can be changed without administrator rights: it gives settings, never the program to start ----
    private static void SharedConfigRules(string scratch)
    {
        var given = LaunchOptions.Parse(["--sparxie", @"C:\Sparxie\Sparxie.exe", "--profile", "p", "--config-dir", @"D:\External Sparxie", "--exe", @"D:\Games\StarRail.exe"], out var error);
        Check(given is not null, "the two flags of a shared config are read: " + error);
        Same<string?>(@"D:\External Sparxie", given?.ConfigDir, "the shared folder");
        Same<string?>(@"D:\Games\StarRail.exe", given?.GameExe, "the game the application was made for");
        Check(LaunchOptions.Parse(["--sparxie", @"C:\Sparxie\Sparxie.exe", "--profile", "p", "--config-dir", @"D:\External"], out _) is null, "a shared config without the game is refused");
        Check(LaunchOptions.Parse(["--sparxie", @"C:\Sparxie\Sparxie.exe", "--profile", "p", "--exe", @"D:\Games\StarRail.exe"], out _) is null, "the game flag alone is refused");

        using var log = RunLog.Open([Path.Combine(scratch, "shared", "log")], () => new DateTime(2026, 11, 27, 12, 0, 0));
        var changed = new Profile("p", "P", "starRail", @"D:\Elsewhere\StarRail.exe", null);
        Same(@"D:\Games\StarRail.exe", Launcher.PinGame(changed, given!, log).ExecutablePath, "the program the config names is not the one started");
        var plain = LaunchOptions.Parse(["--sparxie", @"C:\Sparxie\Sparxie.exe", "--profile", "p"], out _)!;
        Same(changed.ExecutablePath, Launcher.PinGame(changed, plain, log).ExecutablePath, "the component's own config is taken as it is");
    }
}
