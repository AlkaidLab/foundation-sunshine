using System.Globalization;

namespace Sparxie.Launch;

/// <summary>
/// The window and size the host wants for this session (the Broker's ScreenArgs, field 6 of the profile snapshot). Sparxie turns them into Unity player
/// arguments for the game; a direct start puts the same text on the game's command line. Only integers and fixed tokens are ever built.
/// Unity manual, Standalone Player command-line arguments: -screen-width, -screen-height, -screen-fullscreen 0|1, -popupwindow.
/// </summary>
internal sealed record ScreenArgs(int Width, int Height, string WindowMode)
{
    public const int MinSize = 320;
    public const int MaxSize = 16384;
    public const string Fullscreen = "fullscreen";
    public const string Windowed = "windowed";
    public const string Borderless = "borderless";

    public bool IsEmpty => Width == 0 && Height == 0 && WindowMode.Length == 0;

    /// <summary>Null when valid; else the reason.</summary>
    public static string? Validate(int width, int height, string mode)
    {
        if ((width == 0) != (height == 0)) return "the width and the height must be given together";
        if (width != 0 && (width is < MinSize or > MaxSize || height is < MinSize or > MaxSize)) return $"the size must be {MinSize}-{MaxSize}";
        if (mode.Length != 0 && mode is not (Fullscreen or Windowed or Borderless)) return $"unknown window mode '{mode}'";
        return null;
    }

    /// <summary>The arguments as the game gets them (no leading space); empty when nothing is asked.</summary>
    public string Arguments()
    {
        var parts = new List<string>();
        if (Width != 0)
        {
            parts.Add("-screen-width " + Width.ToString(CultureInfo.InvariantCulture));
            parts.Add("-screen-height " + Height.ToString(CultureInfo.InvariantCulture));
        }
        switch (WindowMode)
        {
            case Fullscreen: parts.Add("-screen-fullscreen 1"); break;
            case Windowed: parts.Add("-screen-fullscreen 0"); break;
            case Borderless: parts.Add("-screen-fullscreen 0"); parts.Add("-popupwindow"); break;
        }
        return string.Join(' ', parts);
    }
}
