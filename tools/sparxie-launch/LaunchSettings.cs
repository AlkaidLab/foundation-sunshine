using System.Text.Json;

namespace Sparxie.Launch;

/// <summary>
/// The launch settings of an official Sparxie profile (config.json "launch", sent as LaunchSettings launch = 6 { width 1, height 2, fullscreen 3, extra_arguments 4,
/// borderless 5 }). Sparxie writes the size and full screen into the game's own settings before the start and puts them back when the game quits; the game's command
/// line gets -popupwindow (borderless) and the person's extra arguments. A size of 0 and a null Fullscreen mean "leave the game's own value".
/// The field number is the one a build with the screen-args capability uses for ScreenArgs, so a Broker is sent one of the two, never both.
/// </summary>
internal sealed record LaunchSettings(int Width = 0, int Height = 0, bool? Fullscreen = null, bool Borderless = false, string ExtraArguments = "")
{
    // Sparxie's own limits (LaunchSettingsRules): its Broker refuses a session outside them
    public const int MinWidth = 640;
    public const int MinHeight = 480;
    public const int MaxSize = 16384;
    public const string BorderlessArgument = "-popupwindow";

    public bool IsEmpty => Width == 0 && Fullscreen is null && !Borderless && ExtraArguments.Length == 0;

    private static bool SizeOk(int width, int height) => width is >= MinWidth and <= MaxSize && height is >= MinHeight and <= MaxSize;

    /// <summary>Null when the block holds nothing.</summary>
    public static LaunchSettings? FromJson(JsonElement e)
    {
        int Int(string key) => e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n) ? n : 0;
        bool? Flag(string key) => e.TryGetProperty(key, out var v) ? v.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => null } : null;
        var extra = e.TryGetProperty("extraArguments", out var x) && x.ValueKind == JsonValueKind.String ? (x.GetString() ?? "").Trim() : "";
        int width = Int("width"), height = Int("height");
        if (!SizeOk(width, height)) width = height = 0;
        var settings = new LaunchSettings(width, height, Flag("fullscreen"), Flag("borderless") ?? false, extra);
        return settings.IsEmpty ? null : settings;
    }

    /// <summary>
    /// The profile's launch settings with the host's screen plan for this stream over them: the plan's size and window mode win, the person's extra arguments stay.
    /// Sparxie fills a missing size with 1920x1080 as soon as full screen or a size is set, so full screen is only said together with a size; a plan whose size
    /// Sparxie would refuse leaves the game's stored size and window to what the host already wrote.
    /// </summary>
    public static LaunchSettings? Merge(LaunchSettings? profile, ScreenArgs? screen)
    {
        if (screen is null || screen.IsEmpty) return profile;
        var merged = profile ?? new LaunchSettings();
        var sizeRefused = screen.Width != 0 && !SizeOk(screen.Width, screen.Height);
        if (screen.Width != 0)
            merged = sizeRefused
                ? merged with { Width = 0, Height = 0, Fullscreen = null }
                : merged with { Width = screen.Width, Height = screen.Height };
        if (screen.WindowMode.Length != 0)
        {
            merged = merged with
            {
                Fullscreen = merged.Width == 0 ? null : screen.WindowMode == ScreenArgs.Fullscreen,
                Borderless = screen.WindowMode == ScreenArgs.Borderless,
            };
        }
        return merged.IsEmpty ? null : merged;
    }

    /// <summary>What Sparxie puts on the game's command line (its LaunchArguments.Build), for a start without Sparxie.</summary>
    public string Arguments()
    {
        var repeated = ExtraArguments.Split(' ', StringSplitOptions.RemoveEmptyEntries).Contains(BorderlessArgument, StringComparer.OrdinalIgnoreCase);
        return string.Join(' ', new[] { Borderless && !repeated ? BorderlessArgument : "", ExtraArguments }.Where(part => part.Length > 0));
    }

    /// <summary>The command line of a direct start: the host's screen arguments (else the profile's borderless window), then the person's extra arguments.</summary>
    public static string DirectArguments(LaunchSettings? profile, ScreenArgs? screen)
    {
        if (screen is null || screen.IsEmpty) return profile?.Arguments() ?? "";
        return string.Join(' ', new[] { screen.Arguments(), profile?.ExtraArguments ?? "" }.Where(part => part.Length > 0));
    }

    public byte[] Encode() => Proto.Message(
        (1, Proto.Int(Width)), (2, Proto.Int(Height)), (3, Proto.Int(Fullscreen switch { true => 1, false => 2, _ => 0 })),
        (4, Proto.String(ExtraArguments)), (5, Proto.Bool(Borderless)));

    public string Describe() =>
        $"size={Width}x{Height} fullscreen={Fullscreen?.ToString() ?? "unchanged"} borderless={Borderless} extraArguments={ExtraArguments}";
}
