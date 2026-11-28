using System.Text.RegularExpressions;

namespace Sparxie.Launch;

/// <summary>
/// The ids a profile's game can have. The first Sparxie generation names Endfield "endfield"; the second one loads it as a plugin and names a profile's game by
/// the plugin's id, ext.&lt;vendor&gt;.&lt;game&gt; (Sparxie.Contracts GameIds.IsValidId: lower case letters, digits and "-", 2-32 characters each).
/// </summary>
internal static partial class GameIds
{
    /// <summary>The plugin that carries Endfield in the second generation (skyhua0224/endfiled-sparxie).</summary>
    public const string EndfieldPlugin = "ext.skyhua0224.endfield";

    public static bool IsEndfield(string game) =>
        string.Equals(game, "endfield", StringComparison.OrdinalIgnoreCase) || string.Equals(game, EndfieldPlugin, StringComparison.Ordinal);

    public static bool IsPlugin(string game) => PluginId().IsMatch(game);

    [GeneratedRegex(@"^ext\.[a-z0-9-]{2,32}\.[a-z0-9-]{2,32}\z", RegexOptions.CultureInvariant)]
    private static partial Regex PluginId();
}
