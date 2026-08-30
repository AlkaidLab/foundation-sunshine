using System.Runtime.InteropServices;

namespace Sunshine.Ds5Sidecar;

internal sealed class WasapiCaptureProbe : IDisposable
{
    private const uint ClsctxAll = 23;
    private const uint DeviceStateActive = 1;
    private const uint AudioClientStreamflagsNoPersist = 0x0008_0000;
    private const uint AudioClientBufferflagsSilent = 0x0000_0002;

    private static readonly Guid MmDeviceEnumeratorClsid =
        new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    private static readonly Guid AudioClientIid =
        new("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
    private static readonly Guid AudioCaptureClientIid =
        new("C8ADBD64-E71E-48a0-A4DE-185C395CD317");

    private readonly IMMDevice _device;
    private readonly IAudioClient _audioClient;
    private readonly IAudioCaptureClient _captureClient;
    private readonly ushort _blockAlign;
    private bool _disposed;

    private WasapiCaptureProbe(
        IMMDevice device, IAudioClient audioClient, IAudioCaptureClient captureClient,
        ushort blockAlign)
    {
        _device = device;
        _audioClient = audioClient;
        _captureClient = captureClient;
        _blockAlign = blockAlign;
    }

    internal ulong CapturedFrames { get; private set; }
    internal ulong NonZeroFrames { get; private set; }

    internal static HashSet<string> GetActiveCaptureEndpointIds()
    {
        var enumerator = CreateEnumerator();
        IMMDeviceCollection? collection = null;
        try
        {
            ThrowIfFailed(enumerator.EnumAudioEndpoints(
                EDataFlow.Capture, DeviceStateActive, out collection));
            ThrowIfFailed(collection.GetCount(out var count));
            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (uint index = 0; index < count; ++index)
            {
                IMMDevice? device = null;
                try
                {
                    ThrowIfFailed(collection.Item(index, out device));
                    ThrowIfFailed(device.GetId(out var id));
                    ids.Add(id);
                }
                finally
                {
                    ReleaseComObject(device);
                }
            }
            return ids;
        }
        finally
        {
            ReleaseComObject(collection);
            ReleaseComObject(enumerator);
        }
    }

    internal static Task<T> RunOnDedicatedThreadAsync<T>(
        string endpointId, Func<WasapiCaptureProbe, T> action)
    {
        var completion = new TaskCompletionSource<T>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                T result;
                using (var probe = Open(endpointId))
                    result = action(probe);
                completion.TrySetResult(result);
            }
            catch (Exception exception)
            {
                completion.TrySetException(exception);
            }
        })
        {
            IsBackground = true,
            Name = "Sunshine WASAPI capture probe",
        };
        thread.Start();
        return completion.Task;
    }

    private static WasapiCaptureProbe Open(string endpointId)
    {
        var enumerator = CreateEnumerator();
        IMMDevice? device = null;
        IAudioClient? audioClient = null;
        IAudioCaptureClient? captureClient = null;
        IntPtr mixFormat = IntPtr.Zero;
        try
        {
            ThrowIfFailed(enumerator.GetDevice(endpointId, out device));
            var audioClientIid = AudioClientIid;
            ThrowIfFailed(device.Activate(
                ref audioClientIid, ClsctxAll, IntPtr.Zero, out var activated));
            audioClient = (IAudioClient)activated;
            ThrowIfFailed(audioClient.GetMixFormat(out mixFormat));
            var blockAlign = checked((ushort)Marshal.ReadInt16(mixFormat, 12));
            if (blockAlign == 0)
                throw new InvalidDataException("WASAPI capture mix format has zero block alignment");
            ThrowIfFailed(audioClient.Initialize(
                AudioClientShareMode.Shared, AudioClientStreamflagsNoPersist,
                1_000_000, 0, mixFormat, IntPtr.Zero));

            var captureClientIid = AudioCaptureClientIid;
            ThrowIfFailed(audioClient.GetService(ref captureClientIid, out var service));
            captureClient = (IAudioCaptureClient)service;
            ThrowIfFailed(audioClient.Start());

            var probe = new WasapiCaptureProbe(
                device, audioClient, captureClient, blockAlign);
            device = null;
            audioClient = null;
            captureClient = null;
            return probe;
        }
        catch
        {
            ReleaseComObject(captureClient);
            ReleaseComObject(audioClient);
            ReleaseComObject(device);
            throw;
        }
        finally
        {
            if (mixFormat != IntPtr.Zero)
                Marshal.FreeCoTaskMem(mixFormat);
            ReleaseComObject(enumerator);
        }
    }

    internal void Drain()
    {
        ThrowIfFailed(_captureClient.GetNextPacketSize(out var packetFrames));
        while (packetFrames != 0)
        {
            ThrowIfFailed(_captureClient.GetBuffer(
                out var data, out var frames, out var flags, out _, out _));
            try
            {
                CapturedFrames += frames;
                if ((flags & AudioClientBufferflagsSilent) == 0 && data != IntPtr.Zero)
                {
                    var bytes = new byte[checked((int)(frames * _blockAlign))];
                    Marshal.Copy(data, bytes, 0, bytes.Length);
                    for (var frame = 0u; frame < frames; ++frame)
                    {
                        if (bytes.AsSpan(checked((int)(frame * _blockAlign)), _blockAlign)
                            .IndexOfAnyExcept((byte)0) >= 0)
                            ++NonZeroFrames;
                    }
                }
            }
            finally
            {
                ThrowIfFailed(_captureClient.ReleaseBuffer(frames));
            }
            ThrowIfFailed(_captureClient.GetNextPacketSize(out packetFrames));
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        _audioClient.Stop();
        ReleaseComObject(_captureClient);
        ReleaseComObject(_audioClient);
        ReleaseComObject(_device);
    }

    private static IMMDeviceEnumerator CreateEnumerator()
    {
        var type = Type.GetTypeFromCLSID(MmDeviceEnumeratorClsid, throwOnError: true)!;
        return (IMMDeviceEnumerator)Activator.CreateInstance(type)!;
    }

    private static void ThrowIfFailed(int result)
    {
        if (result < 0)
            Marshal.ThrowExceptionForHR(result);
    }

    private static void ReleaseComObject(object? value)
    {
        if (value is not null && Marshal.IsComObject(value))
            Marshal.ReleaseComObject(value);
    }

    private enum EDataFlow
    {
        Render,
        Capture,
        All,
    }

    private enum AudioClientShareMode
    {
        Shared,
        Exclusive,
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(
            EDataFlow dataFlow, uint stateMask, out IMMDeviceCollection devices);

        [PreserveSig]
        int GetDefaultAudioEndpoint(EDataFlow dataFlow, int role, out IMMDevice endpoint);

        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);

        [PreserveSig]
        int RegisterEndpointNotificationCallback(IntPtr callback);

        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IntPtr callback);
    }

    [ComImport]
    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint count);

        [PreserveSig]
        int Item(uint index, out IMMDevice device);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        [PreserveSig]
        int Activate(
            ref Guid iid, uint clsctx, IntPtr activationParams,
            [MarshalAs(UnmanagedType.IUnknown)] out object activatedInterface);

        [PreserveSig]
        int OpenPropertyStore(uint access, out IntPtr properties);

        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);

        [PreserveSig]
        int GetState(out uint state);
    }

    [ComImport]
    [Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioClient
    {
        [PreserveSig]
        int Initialize(
            AudioClientShareMode shareMode, uint streamFlags, long bufferDuration,
            long periodicity, IntPtr format, IntPtr audioSessionGuid);

        [PreserveSig]
        int GetBufferSize(out uint bufferFrames);

        [PreserveSig]
        int GetStreamLatency(out long latency);

        [PreserveSig]
        int GetCurrentPadding(out uint paddingFrames);

        [PreserveSig]
        int IsFormatSupported(
            AudioClientShareMode shareMode, IntPtr format, out IntPtr closestMatch);

        [PreserveSig]
        int GetMixFormat(out IntPtr format);

        [PreserveSig]
        int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);

        [PreserveSig]
        int Start();

        [PreserveSig]
        int Stop();

        [PreserveSig]
        int Reset();

        [PreserveSig]
        int SetEventHandle(IntPtr eventHandle);

        [PreserveSig]
        int GetService(
            ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
    }

    [ComImport]
    [Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioCaptureClient
    {
        [PreserveSig]
        int GetBuffer(
            out IntPtr data, out uint frames, out uint flags,
            out ulong devicePosition, out ulong qpcPosition);

        [PreserveSig]
        int ReleaseBuffer(uint frames);

        [PreserveSig]
        int GetNextPacketSize(out uint frames);
    }
}
