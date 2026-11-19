using System.Globalization;

namespace Sparxie.Launch;

/// <summary>The command line the host writes: <c>--sparxie &lt;Sparxie.exe&gt; --profile &lt;id&gt;</c>.</summary>
internal sealed record LaunchOptions(string SparxiePath, string ProfileId, ScreenArgs? Screen = null, string? ScreenWarning = null)
{
    public const string Usage = "usage: sparxie-launch --sparxie <path to Sparxie.exe> --profile <profile id> [--screen-width <w> --screen-height <h>] [--window-mode fullscreen|windowed|borderless]";

    /// <summary>Returns the options, or null with a one-line reason.</summary>
    public static LaunchOptions? Parse(IReadOnlyList<string> args, out string error)
    {
        string? sparxie = null;
        string? profile = null;
        string? screenWidth = null, screenHeight = null, windowMode = null;
        for (var i = 0; i < args.Count; i++)
        {
            var arg = args[i];
            string? inline = null;
            var eq = arg.IndexOf('=');
            if (arg.StartsWith("--", StringComparison.Ordinal) && eq > 2)
            {
                inline = arg[(eq + 1)..];
                arg = arg[..eq];
            }

            switch (arg)
            {
                case "--sparxie":
                case "--profile":
                case "--screen-width":
                case "--screen-height":
                case "--window-mode":
                    string? value = inline;
                    if (value is null)
                    {
                        if (i + 1 >= args.Count)
                        {
                            error = $"{arg} needs a value. {Usage}";
                            return null;
                        }
                        value = args[++i];
                    }
                    switch (arg)
                    {
                        case "--sparxie": sparxie = value; break;
                        case "--profile": profile = value; break;
                        case "--screen-width": screenWidth = value; break;
                        case "--screen-height": screenHeight = value; break;
                        default: windowMode = value; break;
                    }
                    break;
                default:
                    error = $"unknown argument '{args[i]}'. {Usage}";
                    return null;
            }
        }

        if (string.IsNullOrWhiteSpace(sparxie))
        {
            error = $"--sparxie is missing. {Usage}";
            return null;
        }
        if (string.IsNullOrWhiteSpace(profile))
        {
            error = $"--profile is missing. {Usage}";
            return null;
        }

        string full;
        try { full = Path.GetFullPath(sparxie); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            error = $"--sparxie is not a usable path: {ex.Message}";
            return null;
        }
        error = "";
        // the screen arguments never stop a launch: a value that cannot be used is dropped and said so in the log
        ScreenArgs? screen = null;
        string? warning = null;
        if (screenWidth is not null || screenHeight is not null || windowMode is not null)
        {
            var mode = (windowMode ?? "").Trim();
            if (!int.TryParse(screenWidth ?? "0", NumberStyles.None, CultureInfo.InvariantCulture, out var w) || !int.TryParse(screenHeight ?? "0", NumberStyles.None, CultureInfo.InvariantCulture, out var h))
                warning = "the screen size is not a number";
            else if (ScreenArgs.Validate(w, h, mode) is { } why)
                warning = why;
            else if (new ScreenArgs(w, h, mode) is { IsEmpty: false } ok)
                screen = ok;
        }
        return new LaunchOptions(full, profile.Trim(), screen, warning);
    }
}

/// <summary>How the launcher ends. The host and Sunshine see only the number; the log says why.</summary>
internal static class ExitCodes
{
    public const int Exited = 0;          // the game ran and quit
    public const int Usage = 2;           // bad command line
    public const int Config = 3;          // no data dir, no config.json, profile missing or unreadable
    public const int Broker = 4;          // Broker not found, did not start, exited early or never answered
    public const int Rejected = 5;        // Broker refused StartSession
    public const int Rpc = 6;             // a pipe or protocol error mid-session
    public const int SessionFailed = 7;   // Failed / HostCrashed*
    public const int Lost = 8;            // event stream ended without a terminal event
    public const int TimedOut = 9;        // the game never reached Running
    public const int Internal = 10;       // anything unexpected
    public const int Unsupported = 11;    // the profile needs a Sparxie feature the installed build lacks (a session without the touch interface)

    public static int ForState(string state) => state switch
    {
        "Exited" => Exited,
        "Failed" or "HostCrashedBeforeRunning" or "HostCrashedAfterRunning" => SessionFailed,
        _ => -1,
    };

    public static bool IsTerminal(string state) => ForState(state) >= 0;
}
