using System.IO.Pipes;
using System.Threading.Channels;

namespace Sunshine.Ds5Sidecar;

internal sealed class VirtualDeviceHostServer : IAsyncDisposable
{
    private readonly string _pipeName;
    private readonly bool _skipOwnerVerificationForTests;
    private readonly DeviceRegistry _devices;
    private readonly Channel<Protocol.Message> _controlOutgoing;
    private readonly Channel<Protocol.Message> _realtimeOutgoing;
    private readonly SemaphoreSlim _outgoingSignal = new(0, 1);
    private NamedPipeServerStream? _pipe;
    private CancellationTokenSource? _sessionCancellation;

    internal VirtualDeviceHostServer(
        string pipeName, bool enableCompositeMicrophonePrototype = false,
        bool skipOwnerVerificationForTests = false)
    {
        _pipeName = pipeName;
        _skipOwnerVerificationForTests = skipOwnerVerificationForTests;
        _controlOutgoing = Channel.CreateUnbounded<Protocol.Message>(new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false,
        });
        _realtimeOutgoing = Channel.CreateBounded<Protocol.Message>(new BoundedChannelOptions(32)
        {
            SingleReader = true,
            SingleWriter = false,
            FullMode = BoundedChannelFullMode.DropOldest,
        });
        _devices = new DeviceRegistry(Emit, enableCompositeMicrophonePrototype);
    }

    internal async Task RunAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                _pipeName,
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.WriteThrough |
                PipeOptions.CurrentUserOnly | PipeOptions.FirstPipeInstance,
                64 * 1024,
                64 * 1024);
            _pipe = pipe;
            await pipe.WaitForConnectionAsync(stoppingToken);
            if (!_skipOwnerVerificationForTests && !OwnerVerification.ClientIsElevated(pipe))
            {
                // Core does not retry a failed launch within a session, so a
                // rejected client must not burn the sidecar: drop the connection
                // and keep waiting for the real owner.
                Console.Error.WriteLine("Rejected a non-elevated DualSense sidecar pipe client");
                _pipe = null;
                continue;
            }
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
            _sessionCancellation = linked;
            var writer = WriteLoopAsync(pipe, linked.Token);
            try
            {
                await ReadLoopAsync(pipe, linked.Token);
            }
            catch (EndOfStreamException)
            {
                // The owning Sunshine process disconnected. The sidecar exits after
                // destroying every device instead of becoming an orphan service.
            }
            catch (IOException) when (!pipe.IsConnected)
            {
                // Windows may surface a broken owner pipe as ERROR_BROKEN_PIPE or
                // ERROR_NO_DATA instead of a zero-byte read. Treat both as EOF.
            }
            finally
            {
                try
                {
                    linked.Cancel();
                    _controlOutgoing.Writer.TryComplete();
                    _realtimeOutgoing.Writer.TryComplete();
                    try
                    {
                        await writer;
                    }
                    catch (OperationCanceledException)
                    {
                        // Expected when the owner or host cancellation stops the writer.
                    }
                    catch (IOException) when (linked.IsCancellationRequested || !pipe.IsConnected)
                    {
                        // A pending WriteAsync/FlushAsync reports a broken owner pipe as
                        // IOException on Windows. Cleanup must still destroy every device.
                    }
                }
                finally
                {
                    _devices.ReleaseAllDevices();
                    _pipe = null;
                    _sessionCancellation = null;
                }
            }

            // The owner session ended; exit instead of serving a second owner.
            return;
        }
    }

    private async Task ReadLoopAsync(Stream pipe, CancellationToken cancellationToken)
    {
        var helloSeen = false;
        var headerBytes = new byte[Protocol.HeaderSize];
        while (!cancellationToken.IsCancellationRequested)
        {
            await ReadExactlyAsync(pipe, headerBytes, cancellationToken);
            var header = Protocol.DecodeHeader(headerBytes);
            var payload = new byte[header.PayloadLength];
            if (payload.Length != 0)
                await ReadExactlyAsync(pipe, payload, cancellationToken);

            if (!helloSeen && header.Type != Protocol.MessageType.Hello)
                throw new InvalidDataException("hello must be the first sidecar message");

            try
            {
                switch (header.Type)
                {
                    case Protocol.MessageType.Hello:
                        if (helloSeen || payload.Length != 4)
                            throw new InvalidDataException("Invalid hello payload");
                        helloSeen = true;
                        Emit(new Protocol.Message(
                            Protocol.MessageType.HelloReply,
                            header.RequestId,
                            Protocol.UInt32((uint)_devices.Capabilities)));
                        break;
                    case Protocol.MessageType.Attach:
                        _devices.Attach(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.Detach:
                        _devices.Detach(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.InputState:
                        _devices.GetController(payload).SubmitInput(payload);
                        break;
                    case Protocol.MessageType.Touch:
                        _devices.GetController(payload).SubmitTouch(payload);
                        break;
                    case Protocol.MessageType.Motion:
                        _devices.GetController(payload).SubmitMotion(payload);
                        break;
                    case Protocol.MessageType.Battery:
                        _devices.GetController(payload).SubmitBattery(payload);
                        break;
                    case Protocol.MessageType.MicCreate:
                        RequireRequestId(header.RequestId, required: true);
                        _devices.CreateMicrophone(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.MicPcm:
                        RequireRequestId(header.RequestId, required: false);
                        _devices.SubmitMicrophonePcm(payload);
                        break;
                    case Protocol.MessageType.MicFlush:
                        RequireRequestId(header.RequestId, required: true);
                        _devices.FlushMicrophone(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.MicDestroy:
                        RequireRequestId(header.RequestId, required: true);
                        _devices.DestroyMicrophone(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.Shutdown:
                        _sessionCancellation?.Cancel();
                        return;
                    default:
                        throw new InvalidDataException($"Unsupported sidecar message {header.Type}");
                }
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Emit(new Protocol.Message(
                    Protocol.MessageType.Error,
                    header.RequestId,
                    Protocol.ErrorPayload(-1, ex.Message)));
            }
        }
    }

    private void Emit(Protocol.Message message)
    {
        var written = message.Type is Protocol.MessageType.HapticsPcm
            ? _realtimeOutgoing.Writer.TryWrite(message)
            : _controlOutgoing.Writer.TryWrite(message);
        if (written)
        {
            try { _outgoingSignal.Release(); }
            catch (SemaphoreFullException) { /* One wakeup drains the complete bounded queue. */ }
        }
    }

    private static void RequireRequestId(uint requestId, bool required)
    {
        if ((required && requestId == 0) || (!required && requestId != 0))
            throw new InvalidDataException(required
                ? "Microphone control messages require a non-zero request id"
                : "Microphone PCM messages require request id zero");
    }

    private async Task WriteLoopAsync(Stream pipe, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await _outgoingSignal.WaitAsync(cancellationToken);
            while (!cancellationToken.IsCancellationRequested)
            {
                Protocol.Message message;
                if (_controlOutgoing.Reader.TryRead(out message))
                {
                    // Reliable request replies always take priority over feedback.
                }
                else if (_realtimeOutgoing.Reader.TryRead(out message))
                {
                    // High-rate audio/feedback is bounded and may be superseded.
                }
                else
                {
                    break;
                }

                var frame = Protocol.Encode(message);
                await pipe.WriteAsync(frame, cancellationToken);
                await pipe.FlushAsync(cancellationToken);
                if (message.Type == Protocol.MessageType.AudioPolicyViolation)
                {
                    // Flush the reason before ending the owner session. Core
                    // consumes it and relaunches this controller in HID-only
                    // mode, so input survives while suspect PCM is disabled.
                    _sessionCancellation?.Cancel();
                    return;
                }
            }
        }
    }

    private static async Task ReadExactlyAsync(Stream stream, Memory<byte> destination, CancellationToken cancellationToken)
    {
        var read = 0;
        while (read < destination.Length)
        {
            var count = await stream.ReadAsync(destination[read..], cancellationToken);
            if (count == 0)
                throw new EndOfStreamException();
            read += count;
        }
    }

    public ValueTask DisposeAsync()
    {
        _sessionCancellation?.Cancel();
        _devices.Dispose();
        _outgoingSignal.Dispose();
        _pipe?.Dispose();
        return ValueTask.CompletedTask;
    }
}
