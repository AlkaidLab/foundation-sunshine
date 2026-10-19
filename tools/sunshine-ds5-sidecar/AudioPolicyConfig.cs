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

    /// <summary>Reads the device property store; missing or differently typed values are not configured.</summary>
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

    /// <summary>Copies a complete native format before clearing its owning property variant.</summary>
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

    /// <summary>Writes a device property using the Windows 10+ ABI and propagates native failure.</summary>
    internal void SetUInt32(string endpointId, PROPERTYKEY key, uint number)
    {
        PROPVARIANT value = default;
        value.vt = VARENUM.VT_UI4;
        value.uintVal = number;
        Marshal.ThrowExceptionForHR(_policy.SetPropertyValue(endpointId, false, ref key, ref value));
    }

    /// <summary>Commits matching endpoint and mix formats while the stack value remains valid.</summary>
    internal unsafe void SetFormat(string endpointId, WAVEFORMATEXTENSIBLE format) =>
        Marshal.ThrowExceptionForHR(_policy.SetDeviceFormat(endpointId, (IntPtr)(&format), (IntPtr)(&format)));

    /// <summary>Releases only this adapter's policy reference.</summary>
    public void Dispose() => CoreAudio.Release(_policy);

    [ComImport]
    [Guid("F8679F50-850A-41CF-9C72-430F290290C8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPolicyConfig
    {
        // Earlier slots are required to preserve the ABI, even when unused.
        /// <summary>Retains the native mix-format slot preceding the policy property methods.</summary>
        [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr format);
        /// <summary>Retains the native device-format slot and its default-format selector.</summary>
        [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, int defaults, IntPtr format);
        /// <summary>Retains the reset-format slot so later method offsets match Windows.</summary>
        [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id);
        /// <summary>Passes endpoint and mix format pointers to the native policy service.</summary>
        [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr endpointFormat, IntPtr mixFormat);
        /// <summary>Retains the native period-query slot preceding the property methods.</summary>
        [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, int defaults, IntPtr period, IntPtr minimum);
        /// <summary>Retains the native period-update slot preceding the property methods.</summary>
        [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr period);
        /// <summary>Retains the native sharing-mode query slot preceding the property methods.</summary>
        [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
        /// <summary>Retains the native sharing-mode update slot preceding the property methods.</summary>
        [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
        // BOOL effectsStore is part of the Windows 10+ ABI; omitting it shifts pointers.
        /// <summary>Reads a property with the explicit Windows 10+ effects-store selector.</summary>
        [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id,
            [MarshalAs(UnmanagedType.Bool)] bool effectsStore, ref PROPERTYKEY key, ref PROPVARIANT value);
        /// <summary>Writes a property with the explicit Windows 10+ effects-store selector.</summary>
        [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id,
            [MarshalAs(UnmanagedType.Bool)] bool effectsStore, ref PROPERTYKEY key, ref PROPVARIANT value);
    }
}
