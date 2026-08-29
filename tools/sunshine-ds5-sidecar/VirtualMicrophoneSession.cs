using System.Buffers;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal interface IVirtualMicrophoneInput
{
    event Action<bool>? StreamingChanged;
    int Channels { get; }
    int SampleRateHz { get; }
    int BitsPerSample { get; }
    bool IsStreaming { get; }
    int BufferedBytes { get; }
    int Submit(ReadOnlySpan<byte> pcm);
}

internal sealed class HidMaestroMicrophoneInput : IVirtualMicrophoneInput, IDisposable
{
    private readonly HMMicrophoneInput _input;

    internal HidMaestroMicrophoneInput(HMMicrophoneInput input)
    {
        _input = input;
        _input.StreamingChanged += OnStreamingChanged;
    }

    public event Action<bool>? StreamingChanged;
    public int Channels => _input.Channels;
    public int SampleRateHz => _input.SampleRateHz;
    public int BitsPerSample => _input.BitsPerSample;
    public bool IsStreaming => _input.IsStreaming;
    public int BufferedBytes => _input.BufferedBytes;
    public int Submit(ReadOnlySpan<byte> pcm) => _input.Submit(pcm);

    private void OnStreamingChanged(HMMicrophoneInput _, bool streaming) =>
        StreamingChanged?.Invoke(streaming);

    public void Dispose() => _input.StreamingChanged -= OnStreamingChanged;
}

internal sealed class VirtualMicrophoneSession : IDisposable
{
    private const int PumpFrames = 480;
    private const int SubmitFailureLimit = 10;
    private readonly IVirtualMicrophoneInput _input;
    private readonly IDisposable? _inputSubscription;
    private readonly Action _disposeDevice;
    private readonly Action<Protocol.Message> _emit;
    private readonly MicrophonePcmQueue _queue = new();
    private readonly CancellationTokenSource _stopping = new();
    private readonly object _stateSync = new();
    private readonly Task? _pumpTask;
    private DefaultAudioEndpointGuard? _audioEndpointGuard;
    private bool _remoteActive;
    private bool _hostStreaming;
    private bool _faulted;
    private bool _disposed;
    private uint _underruns;
    private uint _submitErrors;
    private uint _pumpDroppedFrames;
    private int _lastError;
    private int _consecutiveSubmitFailures;
    private long _lastStatusTick;

    internal VirtualMicrophoneSession(
        HMController controller, uint generation, Action<Protocol.Message> emit) :
        this(CreateInput(controller), controller.Dispose, generation, emit,
            startPump: true, monitorDefaultAudio: true)
    {
    }

    internal VirtualMicrophoneSession(
        IVirtualMicrophoneInput input, Action disposeDevice, uint generation,
        Action<Protocol.Message> emit, bool startPump,
        bool monitorDefaultAudio = false)
    {
        _input = input;
        _inputSubscription = input as IDisposable;
        _disposeDevice = disposeDevice;
        _emit = emit;
        Generation = generation;
        try
        {
            ValidateFormat(input);
            _queue.Reset(generation);
            _hostStreaming = input.IsStreaming;
            input.StreamingChanged += OnStreamingChanged;
            if (monitorDefaultAudio)
                _audioEndpointGuard = new DefaultAudioEndpointGuard(_ => RecordDeviceFault(-3));
            if (startPump)
                _pumpTask = Task.Run(PumpLoopAsync);
        }
        catch
        {
            input.StreamingChanged -= OnStreamingChanged;
            _audioEndpointGuard?.Dispose();
            _inputSubscription?.Dispose();
            _queue.Dispose();
            _stopping.Dispose();
            throw;
        }
    }

    internal uint Generation { get; }

    internal void PublishStatus() => EmitStatus(force: true);

    internal void Enqueue(Protocol.MicPcmPacket packet)
    {
        ThrowIfDisposed();
        if (_queue.Enqueue(packet) == MicrophonePcmQueue.EnqueueResult.GenerationMismatch)
        {
            EmitStatus(force: true);
            return;
        }

        bool stateChanged;
        lock (_stateSync)
        {
            var remoteActive = !packet.Flags.HasFlag(Protocol.MicPcmFlags.StreamEnd);
            stateChanged = remoteActive != _remoteActive;
            _remoteActive = remoteActive;
        }
        EmitStatus(force: stateChanged);
    }

    internal void Flush()
    {
        ThrowIfDisposed();
        _queue.Flush();
        lock (_stateSync)
            _remoteActive = false;
        EmitStatus(force: true);
    }

    internal void PumpOnce()
    {
        lock (_stateSync)
        {
            if (_disposed || _faulted || !_hostStreaming)
                return;
        }

        int bufferedBytes;
        try
        {
            bufferedBytes = Math.Max(0, _input.BufferedBytes);
        }
        catch (Exception error)
        {
            RecordSubmitFailure(error.HResult);
            return;
        }

        var targetBytes = checked(PumpFrames * _input.Channels * 2 * 2);
        if (bufferedBytes >= targetBytes)
        {
            EmitStatus(force: false);
            return;
        }

        if (_queue.TryDequeue(out var block) && block is not null)
        {
            using (block)
                SubmitFrames(block.Pcm, block.FrameCount);
        }
        else
        {
            lock (_stateSync)
                _underruns = SaturatingIncrement(_underruns);
            SubmitFrames(ReadOnlySpan<byte>.Empty, PumpFrames);
        }
        EmitStatus(force: false);
    }

    internal Protocol.MicrophoneStatus GetStatus()
    {
        lock (_stateSync)
        {
            var state = _faulted
                ? Protocol.MicrophoneState.DeviceFaulted
                : _remoteActive
                    ? Protocol.MicrophoneState.RemoteActive
                    : _hostStreaming
                        ? Protocol.MicrophoneState.HostCapturing
                        : Protocol.MicrophoneState.Idle;
            var dropped = SaturatingAdd(_queue.DroppedFrames, _pumpDroppedFrames);
            var buffered = SaturatingAdd(
                (uint)Math.Max(0, _queue.BufferedBytes),
                _hostStreaming ? GetRuntimeBufferedBytes() : 0);
            return new Protocol.MicrophoneStatus(
                Generation, state, _hostStreaming, buffered, _underruns,
                dropped, _submitErrors, _lastError);
        }
    }

    private async Task PumpLoopAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(5));
        try
        {
            while (await timer.WaitForNextTickAsync(_stopping.Token))
                PumpOnce();
        }
        catch (OperationCanceledException) when (_stopping.IsCancellationRequested)
        {
            // Expected during device destroy or owner shutdown.
        }
    }

    private void SubmitFrames(ReadOnlySpan<byte> monoPcm, int frameCount)
    {
        var outputLength = checked(frameCount * _input.Channels * 2);
        var output = ArrayPool<byte>.Shared.Rent(outputLength);
        try
        {
            var destination = output.AsSpan(0, outputLength);
            if (monoPcm.IsEmpty)
            {
                destination.Clear();
            }
            else if (_input.Channels == 1)
            {
                monoPcm.CopyTo(destination);
            }
            else
            {
                for (var frame = 0; frame < frameCount; ++frame)
                {
                    var low = monoPcm[frame * 2];
                    var high = monoPcm[frame * 2 + 1];
                    var offset = frame * 4;
                    destination[offset] = low;
                    destination[offset + 1] = high;
                    destination[offset + 2] = low;
                    destination[offset + 3] = high;
                }
            }

            int accepted;
            try
            {
                accepted = _input.Submit(destination);
            }
            catch (Exception error)
            {
                RecordSubmitFailure(error.HResult, (uint)frameCount);
                return;
            }

            if (accepted != outputLength)
            {
                var acceptedBytes = Math.Clamp(accepted, 0, outputLength);
                var bytesPerFrame = _input.Channels * 2;
                var droppedFrames = (uint)((outputLength - acceptedBytes + bytesPerFrame - 1) /
                                           bytesPerFrame);
                RecordSubmitFailure(-2, droppedFrames);
                return;
            }
            lock (_stateSync)
                _consecutiveSubmitFailures = 0;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(output);
        }
    }

    private void RecordSubmitFailure(int error, uint droppedFrames = 0)
    {
        lock (_stateSync)
        {
            _submitErrors = SaturatingIncrement(_submitErrors);
            _pumpDroppedFrames = SaturatingAdd(_pumpDroppedFrames, droppedFrames);
            _lastError = error;
            _consecutiveSubmitFailures++;
            if (_consecutiveSubmitFailures >= SubmitFailureLimit)
                _faulted = true;
        }
        EmitStatus(force: true);
    }

    private void RecordDeviceFault(int error)
    {
        lock (_stateSync)
        {
            if (_disposed)
                return;
            _faulted = true;
            _lastError = error;
        }
        EmitStatus(force: true);
    }

    private void OnStreamingChanged(bool streaming)
    {
        bool stateChanged;
        lock (_stateSync)
        {
            stateChanged = streaming != _hostStreaming;
            _hostStreaming = streaming;
        }
        if (!stateChanged)
            return;
        if (!streaming)
            _queue.Flush();
        EmitStatus(force: true);
    }

    private void EmitStatus(bool force)
    {
        var now = Environment.TickCount64;
        lock (_stateSync)
        {
            if (_disposed || (!force && now - _lastStatusTick < 1_000))
                return;
            _lastStatusTick = now;
        }
        _emit(new Protocol.Message(
            Protocol.MessageType.MicStatus, 0, Protocol.EncodeMicStatus(GetStatus())));
    }

    private uint GetRuntimeBufferedBytes()
    {
        try
        {
            return (uint)Math.Max(0, _input.BufferedBytes);
        }
        catch
        {
            return 0;
        }
    }

    private static HidMaestroMicrophoneInput CreateInput(HMController controller)
    {
        var microphone = controller.UsbAudio?.Microphone
            ?? throw new InvalidOperationException("HIDMaestro controller has no USB microphone input");
        return new HidMaestroMicrophoneInput(microphone);
    }

    private static void ValidateFormat(IVirtualMicrophoneInput input)
    {
        if (input.SampleRateHz != Protocol.MicrophoneSampleRateHz ||
            input.BitsPerSample != Protocol.MicrophoneBitsPerSample ||
            input.Channels is not (1 or 2))
        {
            throw new InvalidOperationException(
                $"Unsupported HIDMaestro microphone format: {input.SampleRateHz} Hz, " +
                $"{input.Channels} channel(s), {input.BitsPerSample}-bit");
        }
    }

    private void ThrowIfDisposed()
    {
        lock (_stateSync)
        {
            if (_disposed)
                throw new ObjectDisposedException(nameof(VirtualMicrophoneSession));
        }
    }

    private static uint SaturatingIncrement(uint value) =>
        value == uint.MaxValue ? value : value + 1;

    private static uint SaturatingAdd(uint value, uint addition) =>
        uint.MaxValue - value < addition ? uint.MaxValue : value + addition;

    public void Dispose()
    {
        lock (_stateSync)
        {
            if (_disposed)
                return;
            _disposed = true;
        }
        _stopping.Cancel();
        if (_pumpTask is not null)
        {
            try { _pumpTask.GetAwaiter().GetResult(); }
            catch (OperationCanceledException) { }
        }
        Exception? cleanupError = null;
        TryCleanup(() => _audioEndpointGuard?.Dispose(), ref cleanupError);
        _audioEndpointGuard = null;
        TryCleanup(() => _input.StreamingChanged -= OnStreamingChanged, ref cleanupError);
        TryCleanup(_queue.Dispose, ref cleanupError);
        TryCleanup(() => _inputSubscription?.Dispose(), ref cleanupError);
        TryCleanup(_disposeDevice, ref cleanupError);
        TryCleanup(_stopping.Dispose, ref cleanupError);
        if (cleanupError is not null)
            throw new InvalidOperationException("Virtual microphone cleanup failed", cleanupError);
    }

    private static void TryCleanup(Action action, ref Exception? firstError)
    {
        try { action(); }
        catch (Exception error) { firstError ??= error; }
    }
}
