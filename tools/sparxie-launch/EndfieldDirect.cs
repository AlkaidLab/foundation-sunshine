namespace Sparxie.Launch;

/// <summary>
/// <c>sparxie-launch --endfield-direct &lt;Endfield.exe&gt; [--parent-source &lt;image&gt;]</c>: the host's direct start of an Endfield application that has no Sparxie session.
/// The game is created under a decoy parent (<see cref="DecoyParent"/>, the Collapse Launcher Endfield plugin's method) and the launcher stays alive while it runs,
/// so Sunshine tracks it as the app, exactly as in a Sparxie session.
/// Everything after a <c>--</c> separator is the game's command line (the host's screen plan: <c>-screen-width W -screen-height H -screen-fullscreen 0|1 [-popupwindow]</c>);
/// it is built by the host from integers and fixed tokens only, and anything else is refused here too.
/// </summary>
internal static class EndfieldDirect
{
    public const string Flag = "--endfield-direct";
    public const string Usage = "usage: sparxie-launch --endfield-direct <path to Endfield.exe> [--parent-source <image>] [-- <screen arguments>]";
    public const string Separator = "--";

    public static bool IsRequested(IReadOnlyList<string> args) =>
        args.Any(a => a == Flag || a.StartsWith(Flag + "=", StringComparison.Ordinal));

    /// <summary>The game's arguments the host may send after the separator: only its own integer/token plan. Null when valid; else the reason.</summary>
    public static string? ValidateGameArguments(IReadOnlyList<string> tokens)
    {
        for (var i = 0; i < tokens.Count; i++)
        {
            switch (tokens[i])
            {
                case "-screen-width" or "-screen-height":
                    if (i + 1 >= tokens.Count || !int.TryParse(tokens[i + 1], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var n) || n is < ScreenArgs.MinSize or > ScreenArgs.MaxSize)
                        return $"{tokens[i]} needs a whole number of {ScreenArgs.MinSize}-{ScreenArgs.MaxSize}";
                    i++;
                    break;
                case "-screen-fullscreen":
                    if (i + 1 >= tokens.Count || tokens[i + 1] is not ("0" or "1")) return "-screen-fullscreen needs 0 or 1";
                    i++;
                    break;
                case "-popupwindow":
                    break;
                default:
                    return $"'{tokens[i]}' is not a screen argument";
            }
        }
        return null;
    }

    /// <summary>Returns the game path, the optional decoy source and the game's arguments (empty when none), or null with a one-line reason.</summary>
    public static (string Exe, string? ParentSource, string GameArguments)? Parse(IReadOnlyList<string> args, out string error)
    {
        string? exe = null, source = null;
        var gameArguments = "";
        for (var i = 0; i < args.Count; i++)
        {
            var arg = args[i];
            if (arg == Separator)
            {
                var rest = args.Skip(i + 1).ToList();
                var problem = ValidateGameArguments(rest);
                if (problem is not null)
                {
                    error = $"the game arguments after '{Separator}': {problem}. {Usage}";
                    return null;
                }
                gameArguments = string.Join(' ', rest);
                break;
            }
            string? inline = null;
            var eq = arg.IndexOf('=');
            if (arg.StartsWith("--", StringComparison.Ordinal) && eq > 2)
            {
                inline = arg[(eq + 1)..];
                arg = arg[..eq];
            }

            if (arg is not (Flag or "--parent-source"))
            {
                error = $"unknown argument '{args[i]}'. {Usage}";
                return null;
            }

            var value = inline;
            if (value is null)
            {
                if (i + 1 >= args.Count)
                {
                    error = $"{arg} needs a value. {Usage}";
                    return null;
                }
                value = args[++i];
            }
            if (arg == Flag) exe = value; else source = value;
        }

        if (string.IsNullOrWhiteSpace(exe))
        {
            error = $"{Flag} needs the game's path. {Usage}";
            return null;
        }
        try { exe = Path.GetFullPath(exe); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            error = $"{Flag} is not a usable path: {ex.Message}";
            return null;
        }
        error = "";
        return (exe, string.IsNullOrWhiteSpace(source) ? null : source, gameArguments);
    }

    public static async Task<int> RunAsync(IReadOnlyList<string> args, RunLog log, bool elevated)
    {
        var parsed = Parse(args, out var error);
        if (parsed is null)
        {
            return Launcher.Finish(log, ExitCodes.Usage, error);
        }

        var (exe, source, gameArguments) = parsed.Value;
        log.Info($"endfield direct start: {exe}" + (gameArguments.Length > 0 ? $", arguments {gameArguments}" : ""));
        var profile = new Profile("endfield-direct", "Endfield", "endfield", exe, null);
        var code = await DirectLaunch.RunAsync(profile, log, elevated, Environment.CurrentDirectory, source, gameArguments: gameArguments).ConfigureAwait(false);
        return Launcher.Finish(log, code, "endfield direct start: the game ended");
    }
}
