using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;

namespace HIDMaestro.Internal.Usbip;

// Device creation is a consumer of the system-wide transport. Installation,
// replacement, restart and owner stamping belong to Sunshine's manager.
internal static class SharedUsbipTransport
{
    private static readonly JsonElement Contract = LoadContract();
    /// <summary>Reports readiness without converting a compatibility failure into automatic driver repair.</summary>
    internal static bool IsCompatible
    {
        get
        {
            try { return EnsureReady(); }
            catch (InvalidOperationException) { return false; }
        }
    }
    /// <summary>Retains the embedded transport contract independently of its JSON document lifetime.</summary>
    private static JsonElement LoadContract()
    {
        using var stream = typeof(SharedUsbipTransport).Assembly
            .GetManifestResourceStream("Foundation.ComponentCompatibility.json")
            ?? throw new InvalidOperationException("USBIP-SETUP-003: component compatibility contract is missing");
        using var document = JsonDocument.Parse(stream);
        return document.RootElement.GetProperty("usbip").Clone();
    }

    /// <summary>Rejects missing or mismatched drivers without initiating installation or repair.</summary>
    internal static void ValidateState(bool available, string? driverHash)
    {
        if (!available)
            throw new InvalidOperationException(
                "USBIP-SETUP-001: shared USB/IP is unavailable; repair it in Sunshine component management");
        if (!string.Equals(driverHash, Contract.GetProperty("driver_sha256").GetString(), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                $"USBIP-EXEC-006: the shared driver does not match USB/IP {Contract.GetProperty("version").GetString()}; repair it in Sunshine component management");
    }

    /// <summary>Checks interface presence and hashes the registered driver using read-only access.</summary>
    internal static bool EnsureReady()
    {
        string? hash = null;
        var available = VhciClient.IsAvailable();
        if (available)
        {
            var path = UsbipDriverInstaller.InstalledUdeImagePath();
            if (path is not null)
            {
                try
                {
                    using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete);
                    hash = Convert.ToHexString(SHA256.HashData(stream));
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }
        ValidateState(available, hash);
        return true;
    }
}
