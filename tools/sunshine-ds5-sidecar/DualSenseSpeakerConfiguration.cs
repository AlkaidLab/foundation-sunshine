using System.Runtime.InteropServices;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Commits the quadraphonic speaker topology for the active virtual DualSense
/// render endpoint. This is the programmatic equivalent of choosing
/// Quadraphonic and completing mmsys.cpl's speaker setup wizard.
/// </summary>
internal static class DualSenseSpeakerConfiguration
{
    private const uint DeviceStateActive = 0x00000001;
    private const uint ClsctxAll = 23;
    private const uint DsSpeakerQuad = 3;
    private const uint DsSpeakerConfigMask = 0x0000FFFF;
    private static readonly Guid MmDeviceEnumeratorClass =
        new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    private static readonly Guid DirectSound8Interface =
        new("C50A7E93-F395-4834-9EF6-7FA99DE50966");

    internal static bool Ensure(TimeSpan timeout, out bool changed)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            if (TryConfigureVirtualEndpoint(out changed))
            {
                return true;
            }
            if (DateTime.UtcNow >= deadline)
                break;
            Thread.Sleep(50);
        }
        while (true);

        changed = false;
        return false;
    }

    private static bool TryConfigureVirtualEndpoint(out bool changed)
    {
        changed = false;
        IMMDeviceEnumerator? enumerator = null;
        IMMDeviceCollection? endpoints = null;
        try
        {
            var type = Type.GetTypeFromCLSID(MmDeviceEnumeratorClass, throwOnError: true)!;
            enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(type)!;
            ThrowIfFailed(enumerator.EnumAudioEndpoints(DataFlow.Render, DeviceStateActive, out endpoints));
            ThrowIfFailed(endpoints.GetCount(out var count));

            for (uint index = 0; index < count; ++index)
            {
                IMMDevice? endpoint = null;
                try
                {
                    ThrowIfFailed(endpoints.Item(index, out endpoint));
                    ThrowIfFailed(endpoint.GetId(out var endpointId));
                    if (!DefaultAudioEndpointGuard.IsVirtualDualSenseEndpoint(endpointId))
                        continue;

                    changed = EnsureQuadraphonic(endpoint);
                    return true;
                }
                finally
                {
                    Release(endpoint);
                }
            }
            return false;
        }
        finally
        {
            Release(endpoints);
            Release(enumerator);
        }
    }

    private static bool EnsureQuadraphonic(IMMDevice endpoint)
    {
        IDirectSound8? directSound = null;
        try
        {
            var interfaceId = DirectSound8Interface;
            ThrowIfFailed(endpoint.Activate(
                ref interfaceId, ClsctxAll, IntPtr.Zero, out var activated));
            directSound = (IDirectSound8)activated;
            ThrowIfFailed(directSound.GetSpeakerConfig(out var current));
            var changed = !IsQuadraphonic(current);

            // Always submit the topology. The Sound control panel does this
            // when the user clicks Finish even if Quadraphonic was already
            // selected, and some games only recognize the endpoint after that
            // driver-facing configuration transaction has occurred.
            ThrowIfFailed(directSound.SetSpeakerConfig(DsSpeakerQuad));
            ThrowIfFailed(directSound.GetSpeakerConfig(out current));
            if (!IsQuadraphonic(current))
                throw new InvalidOperationException("Windows did not retain the quadraphonic speaker configuration");
            return changed;
        }
        finally
        {
            Release(directSound);
        }
    }

    internal static bool IsQuadraphonic(uint speakerConfig) =>
        (speakerConfig & DsSpeakerConfigMask) == DsSpeakerQuad;

    private static void ThrowIfFailed(int result)
    {
        if (result < 0)
            Marshal.ThrowExceptionForHR(result);
    }

    private static void Release(object? instance)
    {
        if (instance is not null && Marshal.IsComObject(instance))
            Marshal.FinalReleaseComObject(instance);
    }

    private enum DataFlow
    {
        Render = 0,
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(DataFlow dataFlow, uint stateMask, out IMMDeviceCollection endpoints);
        [PreserveSig]
        int GetDefaultAudioEndpoint(DataFlow dataFlow, int role, out IMMDevice endpoint);
        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice endpoint);
        [PreserveSig]
        int RegisterEndpointNotificationCallback(IntPtr callback);
        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IntPtr callback);
    }

    [ComImport]
    [Guid("0BD7A1BE-7A1A-44DB-8397-C0A8FE7AF53E")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint count);
        [PreserveSig]
        int Item(uint index, out IMMDevice endpoint);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        [PreserveSig]
        int Activate(ref Guid interfaceId, uint classContext, IntPtr activationParameters,
                     [MarshalAs(UnmanagedType.IUnknown)] out object instance);
        [PreserveSig]
        int OpenPropertyStore(uint access, out IntPtr properties);
        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig]
        int GetState(out uint state);
    }

    [ComImport]
    [Guid("C50A7E93-F395-4834-9EF6-7FA99DE50966")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDirectSound8
    {
        [PreserveSig]
        int CreateSoundBuffer(IntPtr description, out IntPtr soundBuffer, IntPtr outer);
        [PreserveSig]
        int GetCaps(IntPtr capabilities);
        [PreserveSig]
        int DuplicateSoundBuffer(IntPtr original, out IntPtr duplicate);
        [PreserveSig]
        int SetCooperativeLevel(IntPtr window, uint level);
        [PreserveSig]
        int Compact();
        [PreserveSig]
        int GetSpeakerConfig(out uint speakerConfig);
        [PreserveSig]
        int SetSpeakerConfig(uint speakerConfig);
        [PreserveSig]
        int Initialize(ref Guid deviceGuid);
        [PreserveSig]
        int VerifyCertification(out uint certified);
    }
}
