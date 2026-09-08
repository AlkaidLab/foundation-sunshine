using System.Runtime.InteropServices;

namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Applies the three values represented by Windows' speaker setup wizard to
/// the active virtual DualSense render endpoint: channel layout, available
/// speakers, and full-range speakers.
/// </summary>
internal static class DualSenseSpeakerConfiguration
{
    private const ushort VtUi4 = 19;
    private const ushort VtBlob = 65;
    private const uint QuadraphonicSpeakerMask = 0x00000033;
    private static readonly Guid PolicyConfigClientClass =
        new("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9");
    private static readonly PropertyKey DeviceFormat = new(
        new Guid("F19F064D-082C-4E27-BC73-6882A1BB8E4C"), 0);
    private static readonly PropertyKey PhysicalSpeakers = new(
        new Guid("1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E"), 3);
    private static readonly PropertyKey FullRangeSpeakers = new(
        new Guid("1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E"), 6);

    internal static bool Ensure(TimeSpan timeout, out bool changed)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            if (TryConfigureVirtualEndpoints(out changed))
                return true;
            if (DateTime.UtcNow >= deadline)
                break;
            Thread.Sleep(50);
        }
        while (true);

        changed = false;
        return false;
    }

    private static bool TryConfigureVirtualEndpoints(out bool changed)
    {
        changed = false;
        var endpointIds = DefaultAudioEndpointGuard.GetActiveVirtualDualSenseRenderEndpoints();
        foreach (var endpointId in endpointIds)
        {
            changed |= Apply(endpointId);
        }
        return endpointIds.Count != 0;
    }

    private static bool Apply(string endpointId)
    {
        IPolicyConfig? policy = null;
        try
        {
            var type = Type.GetTypeFromCLSID(PolicyConfigClientClass, throwOnError: true)!;
            policy = (IPolicyConfig)Activator.CreateInstance(type)!;
            var changed = !ReadAndValidate(policy, endpointId);

            SetFormat(policy, endpointId, CreateQuadraphonicFormat());
            SetUInt32(policy, endpointId, PhysicalSpeakers, QuadraphonicSpeakerMask);
            SetUInt32(policy, endpointId, FullRangeSpeakers, QuadraphonicSpeakerMask);

            if (!ReadAndValidate(policy, endpointId))
                throw new InvalidOperationException("Windows did not retain the DualSense speaker configuration");
            return changed;
        }
        finally
        {
            Release(policy);
        }
    }

    private static bool ReadAndValidate(IPolicyConfig policy, string endpointId)
    {
        return ReadUInt32(policy, endpointId, PhysicalSpeakers) == QuadraphonicSpeakerMask &&
               ReadUInt32(policy, endpointId, FullRangeSpeakers) == QuadraphonicSpeakerMask &&
               IsQuadraphonic(ReadFormat(policy, endpointId));
    }

    private static uint ReadUInt32(IPolicyConfig policy, string endpointId, PropertyKey key)
    {
        PropVariant value = default;
        try
        {
            if (policy.GetPropertyValue(endpointId, ref key, ref value) < 0)
                return 0;
            return value.ValueType == VtUi4 ? value.UInt32Value : 0;
        }
        finally
        {
            PropVariantClear(ref value);
        }
    }

    private static WaveFormatExtensible ReadFormat(IPolicyConfig policy, string endpointId)
    {
        PropVariant value = default;
        try
        {
            var key = DeviceFormat;
            if (policy.GetPropertyValue(endpointId, ref key, ref value) < 0)
                return default;
            if (value.ValueType != VtBlob ||
                value.Blob.Size < Marshal.SizeOf<WaveFormatExtensible>() ||
                value.Blob.Data == IntPtr.Zero)
            {
                return default;
            }
            return Marshal.PtrToStructure<WaveFormatExtensible>(value.Blob.Data);
        }
        finally
        {
            PropVariantClear(ref value);
        }
    }

    private static void SetUInt32(
        IPolicyConfig policy, string endpointId, PropertyKey key, uint number)
    {
        var value = PropVariant.FromUInt32(number);
        ThrowIfFailed(policy.SetPropertyValue(endpointId, ref key, ref value));
    }

    private static void SetFormat(
        IPolicyConfig policy, string endpointId, WaveFormatExtensible format)
    {
        var pointer = Marshal.AllocCoTaskMem(Marshal.SizeOf<WaveFormatExtensible>());
        try
        {
            Marshal.StructureToPtr(format, pointer, fDeleteOld: false);
            ThrowIfFailed(policy.SetDeviceFormat(endpointId, pointer, pointer));
        }
        finally
        {
            Marshal.FreeCoTaskMem(pointer);
        }
    }

    internal static WaveFormatExtensible CreateQuadraphonicFormat() => new()
    {
        FormatTag = 0xFFFE,
        Channels = 4,
        SamplesPerSecond = 48000,
        AverageBytesPerSecond = 48000 * 4 * 2,
        BlockAlign = 4 * 2,
        BitsPerSample = 16,
        ExtraSize = 22,
        ValidBitsPerSample = 16,
        ChannelMask = QuadraphonicSpeakerMask,
        SubFormat = new Guid("00000001-0000-0010-8000-00AA00389B71"),
    };

    internal static bool IsQuadraphonic(WaveFormatExtensible format) =>
        format.FormatTag == 0xFFFE &&
        format.Channels == 4 &&
        format.SamplesPerSecond == 48000 &&
        format.BitsPerSample == 16 &&
        format.ChannelMask == QuadraphonicSpeakerMask &&
        format.SubFormat == new Guid("00000001-0000-0010-8000-00AA00389B71");

    internal static bool HasValidInteropLayout() =>
        Marshal.SizeOf<PropertyKey>() == 20 &&
        Marshal.SizeOf<BlobValue>() == 16 &&
        Marshal.SizeOf<PropVariant>() == 24 &&
        Marshal.SizeOf<WaveFormatExtensible>() == 40;

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

    [StructLayout(LayoutKind.Sequential)]
    private struct PropertyKey(Guid formatId, uint propertyId)
    {
        internal Guid FormatId = formatId;
        internal uint PropertyId = propertyId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BlobValue
    {
        internal uint Size;
        internal IntPtr Data;
    }

    [StructLayout(LayoutKind.Explicit, Size = 24)]
    private struct PropVariant
    {
        [FieldOffset(0)]
        internal ushort ValueType;
        [FieldOffset(8)]
        internal uint UInt32Value;
        [FieldOffset(8)]
        internal BlobValue Blob;

        internal static PropVariant FromUInt32(uint value) => new()
        {
            ValueType = VtUi4,
            UInt32Value = value,
        };
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct WaveFormatExtensible
    {
        internal ushort FormatTag;
        internal ushort Channels;
        internal uint SamplesPerSecond;
        internal uint AverageBytesPerSecond;
        internal ushort BlockAlign;
        internal ushort BitsPerSample;
        internal ushort ExtraSize;
        internal ushort ValidBitsPerSample;
        internal uint ChannelMask;
        internal Guid SubFormat;
    }

    [ComImport]
    [Guid("F8679F50-850A-41CF-9C72-430F290290C8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPolicyConfig
    {
        [PreserveSig] int Unused1();
        [PreserveSig] int Unused2();
        [PreserveSig] int Unused3();
        [PreserveSig]
        int SetDeviceFormat(
            [MarshalAs(UnmanagedType.LPWStr)] string endpointId,
            IntPtr endpointFormat, IntPtr mixFormat);
        [PreserveSig] int Unused5();
        [PreserveSig] int Unused6();
        [PreserveSig] int Unused7();
        [PreserveSig] int Unused8();
        [PreserveSig]
        int GetPropertyValue(
            [MarshalAs(UnmanagedType.LPWStr)] string endpointId,
            ref PropertyKey key, ref PropVariant value);
        [PreserveSig]
        int SetPropertyValue(
            [MarshalAs(UnmanagedType.LPWStr)] string endpointId,
            ref PropertyKey key, ref PropVariant value);
        [PreserveSig]
        int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string endpointId, int role);
        [PreserveSig]
        int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string endpointId, short visible);
    }

    [DllImport("ole32.dll")]
    private static extern int PropVariantClear(ref PropVariant value);
}
