using System.Text;
using System.Text.Json;

namespace Sparxie.Launch;

/// <summary>Where the list of a Sparxie build's optional features came from.</summary>
internal enum CapabilitySource
{
    /// <summary>The Broker's own Ping answer (field 3): the most exact source, a Broker that lists them knows what it supports.</summary>
    Ping,

    /// <summary>capabilities.json beside the Broker, written by the package of a build whose Broker predates the Ping field.</summary>
    Manifest,

    /// <summary>Neither of the above: read from the installed files, for builds that report nothing at all.</summary>
    InstalledFiles,
}

/// <summary>
/// What the installed Sparxie can do. Two different situations must not be confused: a Broker that lists its features and leaves one out
/// (the feature is really missing: refuse), and a Broker that lists nothing because it predates the list (look at what is installed).
/// </summary>
internal sealed record BrokerCapabilities(IReadOnlySet<string> Names, CapabilitySource Source, bool HasLaunchArguments = false)
{
    public const string ManifestFileName = "capabilities.json";

    /// <summary>Markers in Sparxie.Contracts.dll (the names of the generated protocol types): a Broker built from them reads the field.</summary>
    private static readonly (string Capability, string Marker)[] ContractMarkers =
    [
        (PingInfo.TouchUiSwitch, "TouchUiEnabled"),
        (PingInfo.ScreenArgsCapability, "ScreenFieldNumber"),
    ];

    public bool Has(string capability) => Names.Contains(capability);

    // The published screen-args fork carries LaunchSettings at 6 and ScreenArgs at 7.
    // Preserve the screen-only layout for builds advertising screen-args without LaunchSettings.
    public int ScreenField(string game)
    {
        if (!Has(PingInfo.ScreenArgsCapability)) return 0;
        if (!HasLaunchArguments) return 6;
        return GameIds.IsPlugin(game) ? 0 : 7; // field 7 carries settings_json on plugin builds
    }

    public override string ToString() =>
        (Names.Count > 0 ? string.Join(",", Names.Order(StringComparer.Ordinal)) : "none")
        + " (" + Source switch
        {
            CapabilitySource.Ping => "reported by the Broker",
            CapabilitySource.Manifest => "from capabilities.json of the install",
            _ => "read from the installed files, the Broker reports none",
        } + "), launch arguments " + (HasLaunchArguments ? "supported" : "unavailable");

    /// <summary>The Ping list when the Broker gives one, else the install's capabilities.json, else what the installed files show.</summary>
    public static BrokerCapabilities Resolve(PingInfo? ping, string brokerPath)
    {
        var directory = Path.GetDirectoryName(brokerPath) ?? ".";
        var contracts = ReadContracts(directory);
        // LaunchSettings is part of the core contract on builds that ship it, including ones whose Ping or manifest never lists it.
        var launchArguments = HasMarker(contracts, "LaunchFieldNumber") && HasMarker(contracts, "ExtraArgumentsFieldNumber");
        if (ping is { Capabilities.Count: > 0 })
        {
            return new BrokerCapabilities(ping.Capabilities.ToHashSet(StringComparer.Ordinal), CapabilitySource.Ping, launchArguments);
        }

        if (ReadManifest(Path.Combine(directory, ManifestFileName)) is { } listed)
        {
            return new BrokerCapabilities(listed, CapabilitySource.Manifest, launchArguments);
        }

        return new BrokerCapabilities(InferFromFiles(directory, contracts), CapabilitySource.InstalledFiles, launchArguments);
    }

    /// <summary>{ "schema": 1, "capabilities": [ ... ] }; anything else (missing, unreadable, another schema) is no manifest.</summary>
    internal static HashSet<string>? ReadManifest(string path)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path, Encoding.UTF8));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object
                || !root.TryGetProperty("schema", out var schema) || schema.ValueKind != JsonValueKind.Number || schema.GetInt32() != 1
                || !root.TryGetProperty("capabilities", out var list) || list.ValueKind != JsonValueKind.Array)
            {
                return null;
            }

            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in list.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.String) return null;
                names.Add(item.GetString()!);
            }

            return names;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException)
        {
            return null;
        }
    }

    /// <summary>
    /// For builds that report nothing: Endfield is there when both Endfield libraries ship; the touch-interface switch and the screen arguments
    /// are there when the protocol types of Sparxie.Contracts.dll carry their fields.
    /// </summary>
    private static HashSet<string> InferFromFiles(string directory, byte[] contracts)
    {
        var names = new HashSet<string>(StringComparer.Ordinal);
        if (File.Exists(Path.Combine(directory, "EndfieldCore.dll")) && File.Exists(Path.Combine(directory, "EndfieldRuntime.dll")))
        {
            names.Add(PingInfo.Endfield);
        }

        foreach (var (capability, marker) in ContractMarkers)
        {
            if (HasMarker(contracts, marker)) names.Add(capability);
        }

        return names;
    }

    private static byte[] ReadContracts(string directory)
    {
        try { return File.ReadAllBytes(Path.Combine(directory, "Sparxie.Contracts.dll")); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return []; }
    }

    private static bool HasMarker(byte[] contracts, string marker) => contracts.AsSpan().IndexOf(Encoding.ASCII.GetBytes(marker)) >= 0;
}
