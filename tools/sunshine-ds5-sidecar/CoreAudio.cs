using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Media.Audio;

namespace Sunshine.Ds5Sidecar;

/// <summary>Owns endpoint enumeration and COM references shared by audio consumers.</summary>
internal static class CoreAudio
{
    /// <summary>Acquires one enumerator reference owned by the caller.</summary>
    internal static IMMDeviceEnumerator CreateEnumerator() =>
        (IMMDeviceEnumerator)new MMDeviceEnumerator();

    /// <summary>Copies the endpoint identity and frees the COM-allocated string.</summary>
    internal static unsafe string GetEndpointId(IMMDevice endpoint)
    {
        endpoint.GetId(out var id).ThrowOnFailure();
        try { return id.ToString(); }
        finally { Marshal.FreeCoTaskMem((IntPtr)id.Value); }
    }

    /// <summary>Snapshots active endpoint identities, releasing every acquired COM reference.</summary>
    internal static IReadOnlyList<string> GetActiveEndpointIds(EDataFlow flow)
    {
        var enumerator = CreateEnumerator();
        IMMDeviceCollection? endpoints = null;
        try
        {
            enumerator.EnumAudioEndpoints(flow, DEVICE_STATE.DEVICE_STATE_ACTIVE, out endpoints).ThrowOnFailure();
            endpoints.GetCount(out var count).ThrowOnFailure();
            var ids = new List<string>();
            for (uint index = 0; index < count; ++index)
            {
                endpoints.Item(index, out var endpoint).ThrowOnFailure();
                try { ids.Add(GetEndpointId(endpoint)); }
                finally { Release(endpoint); }
            }
            return ids;
        }
        finally
        {
            Release(endpoints);
            Release(enumerator);
        }
    }

    /// <summary>Releases this acquisition without invalidating references held by another audio consumer.</summary>
    internal static void Release(object? instance)
    {
        // Core Audio can return a shared RCW. Never force-release other users.
        if (instance is not null && Marshal.IsComObject(instance))
            Marshal.ReleaseComObject(instance);
    }
}
