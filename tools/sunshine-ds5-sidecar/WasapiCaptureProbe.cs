using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Media.Audio;
using Windows.Win32.System.Com;

namespace Sunshine.Ds5Sidecar;

internal sealed unsafe class WasapiCaptureProbe : IDisposable
{
    private const uint AudioClientStreamflagsNoPersist = 0x0008_0000;
    private const uint AudioClientBufferflagsSilent = 0x0000_0002;

    private readonly IMMDevice _device;
    private readonly IAudioClient _audioClient;
    private readonly IAudioCaptureClient _captureClient;
    private readonly ushort _blockAlign;
    private bool _disposed;

    /// <summary>Takes ownership of an initialized capture device, client and service.</summary>
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

    /// <summary>Snapshots identities used to distinguish a new test endpoint from existing microphones.</summary>
    internal static HashSet<string> GetActiveCaptureEndpointIds() =>
        new(CoreAudio.GetActiveEndpointIds(EDataFlow.eCapture), StringComparer.OrdinalIgnoreCase);

    /// <summary>Keeps opening, capture and disposal on one thread and propagates its result asynchronously.</summary>
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

    /// <summary>Initializes shared-mode capture and releases all acquired native resources on failure.</summary>
    private static WasapiCaptureProbe Open(string endpointId)
    {
        var enumerator = CoreAudio.CreateEnumerator();
        IMMDevice? device = null;
        IAudioClient? audioClient = null;
        IAudioCaptureClient? captureClient = null;
        WAVEFORMATEX* mixFormat = null;
        try
        {
            enumerator.GetDevice(endpointId, out device).ThrowOnFailure();
            var audioClientIid = typeof(IAudioClient).GUID;
            device.Activate(
                in audioClientIid, CLSCTX.CLSCTX_ALL, null, out var activated).ThrowOnFailure();
            audioClient = (IAudioClient)activated;
            audioClient.GetMixFormat(out mixFormat).ThrowOnFailure();
            var blockAlign = mixFormat->nBlockAlign;
            if (blockAlign == 0)
                throw new InvalidDataException("WASAPI capture mix format has zero block alignment");
            audioClient.Initialize(
                AUDCLNT_SHAREMODE.AUDCLNT_SHAREMODE_SHARED, AudioClientStreamflagsNoPersist,
                1_000_000, 0, mixFormat, null).ThrowOnFailure();

            // Use the generated native signature: the generic helper routes
            // this classic COM service through the WinRT object marshaller.
            var captureClientIid = typeof(IAudioCaptureClient).GUID;
            audioClient.GetService(&captureClientIid, out var service).ThrowOnFailure();
            captureClient = (IAudioCaptureClient)service;
            audioClient.Start().ThrowOnFailure();

            var probe = new WasapiCaptureProbe(
                device, audioClient, captureClient, blockAlign);
            device = null;
            audioClient = null;
            captureClient = null;
            return probe;
        }
        catch
        {
            CoreAudio.Release(captureClient);
            CoreAudio.Release(audioClient);
            CoreAudio.Release(device);
            throw;
        }
        finally
        {
            if (mixFormat != null)
                Marshal.FreeCoTaskMem((IntPtr)mixFormat);
            CoreAudio.Release(enumerator);
        }
    }

    /// <summary>Counts captured and nonzero frames while returning every acquired packet buffer.</summary>
    internal void Drain()
    {
        _captureClient.GetNextPacketSize(out var packetFrames).ThrowOnFailure();
        while (packetFrames != 0)
        {
            _captureClient.GetBuffer(
                out var data, out var frames, out var flags, out _, out _).ThrowOnFailure();
            try
            {
                CapturedFrames += frames;
                if ((flags & AudioClientBufferflagsSilent) == 0 && data != null)
                {
                    var bytes = new byte[checked((int)(frames * _blockAlign))];
                    Marshal.Copy((IntPtr)data, bytes, 0, bytes.Length);
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
                _captureClient.ReleaseBuffer(frames).ThrowOnFailure();
            }
            _captureClient.GetNextPacketSize(out packetFrames).ThrowOnFailure();
        }
    }

    /// <summary>Stops capture and releases its service, audio client and endpoint once.</summary>
    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        _audioClient.Stop();
        CoreAudio.Release(_captureClient);
        CoreAudio.Release(_audioClient);
        CoreAudio.Release(_device);
    }
}
