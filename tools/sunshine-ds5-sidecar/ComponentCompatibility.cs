using System.Reflection;
using System.Text.Json;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal static class ComponentCompatibility
{
    private static readonly JsonElement Contract = Load();
    internal static string Version => Contract.GetProperty("component_version").GetString()!;
    internal static string TransportPolicy => Contract.GetProperty("usbip").GetProperty("policy").GetString()!;
    internal static string UsbipVersion => Contract.GetProperty("usbip").GetProperty("version").GetString()!;

    private static JsonElement Load()
    {
        using var stream = typeof(ComponentCompatibility).Assembly
            .GetManifestResourceStream("Foundation.ComponentCompatibility.json")
            ?? throw new InvalidOperationException("The component compatibility contract is missing");
        using var document = JsonDocument.Parse(stream);
        return document.RootElement.Clone();
    }

    internal static bool HasManagedTransport => typeof(HMContext).Assembly
        .GetCustomAttributes<AssemblyMetadataAttribute>().Any(attribute =>
            attribute.Key == "FoundationUsbipPolicy" && attribute.Value == TransportPolicy);
}
