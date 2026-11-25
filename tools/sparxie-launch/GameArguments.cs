using System.Text;

namespace Sparxie.Launch;

internal static class GameArguments
{
    private const int MaxLength = 512; // Sparxie's LaunchSettingsRules, shared by config validation and the Broker.
    private const string ZzzDx12Flag = "-use-d3d12";
    private static readonly HashSet<string> ZzzRendererFlags = new(StringComparer.OrdinalIgnoreCase)
    {
        ZzzDx12Flag, "-force-d3d12", "-force-d3d11", "-force-d3d11-no-singlethreaded",
    };

    public static string Resolve(Profile profile, LaunchOptions options, Action<string> warn)
    {
        var zzzDx12 = profile.Game == "zenlessZoneZero" ? options.ZzzDx12 : null;
        var fromApp = Validate(options.ExtraArguments, "Sunshine app", warn);
        var fromProfile = Validate(profile.ExtraArguments, "Sparxie profile", warn);
        if (zzzDx12.HasValue)
        {
            fromApp = WithoutZzzRendererFlags(fromApp);
            fromProfile = WithoutZzzRendererFlags(fromProfile);
        }

        var arguments = zzzDx12 == true ? ZzzDx12Flag : "";
        void Add(string extra, string source)
        {
            var combined = Combine(arguments, extra);
            if (combined.Length <= MaxLength) arguments = combined;
            else warn($"{source} extra launch arguments would exceed Sparxie's {MaxLength}-character limit after merging; ignored");
        }

        Add(fromApp, "Sunshine app");
        if (fromProfile != fromApp) Add(fromProfile, "Sparxie profile");
        return arguments;
    }

    public static string Combine(string first, string second) =>
        first.Length == 0 ? second : second.Length == 0 ? first : first + " " + second;

    private static string Validate(string arguments, string source, Action<string> warn)
    {
        var trimmed = arguments.Trim();
        if (trimmed.Length <= MaxLength && !arguments.Any(char.IsControl)) return trimmed;
        warn($"{source} extra launch arguments must be at most {MaxLength} characters with no control characters; ignored");
        return "";
    }

    private static string WithoutZzzRendererFlags(string arguments)
    {
        // Read Windows argument boundaries to avoid changing a flag-shaped string inside a quoted value.
        // Retain the original spelling and quoting of every other argument.
        var kept = new List<string>();
        for (var pos = 0; pos < arguments.Length;)
        {
            if (arguments[pos] is ' ' or '\t') { pos++; continue; }
            var start = pos;
            var value = new StringBuilder();
            var quoted = false;
            while (pos < arguments.Length && (quoted || arguments[pos] is not (' ' or '\t')))
            {
                var slashes = 0;
                while (pos < arguments.Length && arguments[pos] == '\\') { slashes++; pos++; }
                if (pos < arguments.Length && arguments[pos] == '"')
                {
                    pos++;
                    value.Append('\\', slashes / 2);
                    if (slashes % 2 != 0) value.Append('"');
                    else if (quoted && pos < arguments.Length && arguments[pos] == '"') { value.Append('"'); pos++; }
                    else quoted = !quoted;
                }
                else
                {
                    value.Append('\\', slashes);
                    if (pos == arguments.Length || (!quoted && arguments[pos] is ' ' or '\t')) break;
                    value.Append(arguments[pos++]);
                }
            }
            if (!ZzzRendererFlags.Contains(value.ToString())) kept.Add(arguments[start..pos]);
        }
        return string.Join(' ', kept);
    }
}
