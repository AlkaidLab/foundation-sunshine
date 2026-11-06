using System.Reflection;
using System.Text.Json;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal static class ComponentCompatibility
{
    private static readonly JsonElement Contract = Load();
    /// <summary>Identifies the complete component package expected by the embedded contract.</summary>
    internal static string Version => Contract.GetProperty("component_version").GetString()!;
    /// <summary>Identifies the runtime policy that delegates driver maintenance to the manager.</summary>
    internal static string TransportPolicy => Contract.GetProperty("usbip").GetProperty("policy").GetString()!;
    /// <summary>Exposes the tested transport version rather than inferring compatibility from version ordering.</summary>
    internal static string UsbipVersion => Contract.GetProperty("usbip").GetProperty("version").GetString()!;

    /// <summary>Loads the embedded build contract and fails closed when it is missing.</summary>
    private static JsonElement Load()
    {
        using var stream = typeof(ComponentCompatibility).Assembly
            .GetManifestResourceStream("Foundation.ComponentCompatibility.json")
            ?? throw new InvalidOperationException("The component compatibility contract is missing");
        using var document = JsonDocument.Parse(stream);
        return document.RootElement.Clone();
    }

    /// <summary>Rejects an unpatched Core before a probe or device creation can reach its deployment entry point.</summary>
    internal static bool HasManagedTransport => Marker("FoundationUsbipPolicy") == TransportPolicy;

    /// <summary>Names the payload stage this package was built for. The Core carries
    /// the same marker, so a sidecar paired with a differently trimmed Core is
    /// rejected by the startup check instead of failing later at install time.</summary>
    internal static string PayloadPolicy => Marker("FoundationPayloadPolicy") ?? "";

    /// <summary>Whether this package can deploy the UMDF driver at all. Only a
    /// stage that carries a signed catalog can; the shipped composite-only stage
    /// cannot, so its HID-only profile runs on USB/IP instead.</summary>
    internal static bool HidOnlyDriverDeployable => PayloadPolicy != "composite-only-v1";

    private static string? Marker(string key) => typeof(HMContext).Assembly
        .GetCustomAttributes<AssemblyMetadataAttribute>()
        .FirstOrDefault(attribute => attribute.Key == key)?.Value;
}
