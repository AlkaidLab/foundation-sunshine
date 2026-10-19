using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Media.Audio;
using Windows.Win32.System.Com.StructuredStorage;
using Windows.Win32.System.Variant;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// The only binding to the undocumented Windows 10+ audio policy ABI.
/// Keep this separate from SDK-generated interfaces and test it by live readback.
/// </summary>
internal sealed class AudioPolicyConfig : IDisposable
{
    private readonly IPolicyConfig _policy = (IPolicyConfig)Activator.CreateInstance(
        Type.GetTypeFromCLSID(new Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9"), throwOnError: true)!)!;

    internal uint ReadUInt32(string endpointId, PROPERTYKEY key)
    {
        PROPVARIANT value = default;
        try
        {
            return _policy.GetPropertyValue(endpointId, false, ref key, ref value) >= 0 &&
                   value.vt == VARENUM.VT_UI4 ? value.uintVal : 0;
        }
        finally { PInvoke.PropVariantClear(ref value); }
    }

    internal unsafe WAVEFORMATEXTENSIBLE ReadFormat(string endpointId)
    {
        var key = PInvoke.PKEY_AudioEngine_DeviceFormat;
        PROPVARIANT value = default;
        try
        {
            if (_policy.GetPropertyValue(endpointId, false, ref key, ref value) < 0 ||
                value.vt != VARENUM.VT_BLOB || value.blob.cbSize < sizeof(WAVEFORMATEXTENSIBLE) ||
                value.blob.pBlobData == null)
                return default;
            return *(WAVEFORMATEXTENSIBLE*)value.blob.pBlobData;
        }
        finally { PInvoke.PropVariantClear(ref value); }
    }

    internal void SetUInt32(string endpointId, PROPERTYKEY key, uint number)
    {
        PROPVARIANT value = default;
        value.vt = VARENUM.VT_UI4;
        value.uintVal = number;
        Marshal.ThrowExceptionForHR(_policy.SetPropertyValue(endpointId, false, ref key, ref value));
    }

    internal unsafe void SetFormat(string endpointId, WAVEFORMATEXTENSIBLE format) =>
        Marshal.ThrowExceptionForHR(_policy.SetDeviceFormat(endpointId, (IntPtr)(&format), (IntPtr)(&format)));

    public void Dispose() => CoreAudio.Release(_policy);

    [ComImport]
    [Guid("F8679F50-850A-41CF-9C72-430F290290C8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPolicyConfig
    {
        // Earlier slots are required to preserve the ABI, even when unused.
        [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr format);
        [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, int defaults, IntPtr format);
        [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id);
        [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr endpointFormat, IntPtr mixFormat);
        [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, int defaults, IntPtr period, IntPtr minimum);
        [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr period);
        [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
        [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
        // BOOL effectsStore is part of the Windows 10+ ABI; omitting it shifts pointers.
        [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id,
            [MarshalAs(UnmanagedType.Bool)] bool effectsStore, ref PROPERTYKEY key, ref PROPVARIANT value);
        [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id,
            [MarshalAs(UnmanagedType.Bool)] bool effectsStore, ref PROPERTYKEY key, ref PROPVARIANT value);
    }
}
