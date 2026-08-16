using System.Buffers.Binary;
using System.IO.Pipes;
using System.Threading.Channels;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal sealed class SidecarServer : IAsyncDisposable
{
    private readonly string _pipeName;
    private readonly HMContext _context = new();
    private readonly Dictionary<byte, ControllerSession> _controllers = new();
    private readonly Channel<Protocol.Message> _controlOutgoing;
    private readonly Channel<Protocol.Message> _realtimeOutgoing;
    private readonly SemaphoreSlim _outgoingSignal = new(0, 1);
    private NamedPipeServerStream? _pipe;
    private CancellationTokenSource? _sessionCancellation;

    internal SidecarServer(string pipeName)
    {
        _pipeName = pipeName;
        _context.LoadDefaultProfiles();
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
    }

    internal async Task RunAsync(CancellationToken stoppingToken)
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
                DisposeControllers();
                _pipe = null;
                _sessionCancellation = null;
            }
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
                            Protocol.UInt32((uint)(Protocol.Capability.Hid |
                                                   Protocol.Capability.Output |
                                                   Protocol.Capability.AudioFourChannel |
                                                   Protocol.Capability.AuthoredHapticsPcm |
                                                   Protocol.Capability.Touchpad |
                                                   Protocol.Capability.Motion |
                                                   Protocol.Capability.Battery |
                                                   Protocol.Capability.AdaptiveTriggers))));
                        break;
                    case Protocol.MessageType.Attach:
                        Attach(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.Detach:
                        Detach(header.RequestId, payload);
                        break;
                    case Protocol.MessageType.InputState:
                        GetController(payload).SubmitInput(payload);
                        break;
                    case Protocol.MessageType.Touch:
                        GetController(payload).SubmitTouch(payload);
                        break;
                    case Protocol.MessageType.Motion:
                        GetController(payload).SubmitMotion(payload);
                        break;
                    case Protocol.MessageType.Battery:
                        GetController(payload).SubmitBattery(payload);
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

    private void Attach(uint requestId, ReadOnlySpan<byte> payload)
    {
        // id:u8, controller:u8, profile:u8 (0 HID, 1 composite), reserved:u8
        if (payload.Length != 4)
            throw new InvalidDataException("Invalid attach payload");
        var deviceId = payload[0];
        if (_controllers.ContainsKey(deviceId))
            throw new InvalidOperationException("The requested DS5 device already exists");

        var profileId = payload[2] switch
        {
            0 => "dualsense",
            1 => "dualsense-composite",
            _ => throw new InvalidDataException("Unsupported DS5 profile mode"),
        };
        var profile = _context.GetProfile(profileId)
                      ?? throw new InvalidOperationException($"HIDMaestro profile '{profileId}' is missing");
        if (!profile.RequiresUsbipBackend)
            _context.InstallDriver();
        var controller = _context.CreateController(profile);
        ControllerSession session;
        try
        {
            session = new ControllerSession(deviceId, payload[1], controller, profile, Emit);
        }
        catch
        {
            controller.Dispose();
            throw;
        }
        _controllers.Add(deviceId, session);

        var reply = new byte[8];
        reply[0] = deviceId;
        reply[1] = session.HasAudio ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(reply.AsSpan(4, 4),
            (uint)(Protocol.Capability.Hid |
                   Protocol.Capability.Output |
                   Protocol.Capability.Touchpad |
                   Protocol.Capability.Motion |
                   Protocol.Capability.Battery |
                   Protocol.Capability.AdaptiveTriggers |
                   (session.HasAudio
                       ? Protocol.Capability.AudioFourChannel | Protocol.Capability.AuthoredHapticsPcm
                       : 0)));
        Emit(new Protocol.Message(Protocol.MessageType.AttachReply, requestId, reply));
    }

    private void Detach(uint requestId, ReadOnlySpan<byte> payload)
    {
        if (payload.Length != 1)
            throw new InvalidDataException("Invalid detach payload");
        if (_controllers.Remove(payload[0], out var controller))
            controller.Dispose();
        Emit(new Protocol.Message(Protocol.MessageType.DetachReply, requestId, new[] { payload[0] }));
    }

    private ControllerSession GetController(ReadOnlySpan<byte> payload)
    {
        if (payload.IsEmpty || !_controllers.TryGetValue(payload[0], out var controller))
            throw new InvalidOperationException("The requested DS5 device is not attached");
        return controller;
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

    private void DisposeControllers()
    {
        foreach (var controller in _controllers.Values)
            controller.Dispose();
        _controllers.Clear();
    }

    public ValueTask DisposeAsync()
    {
        _sessionCancellation?.Cancel();
        DisposeControllers();
        _context.Dispose();
        _outgoingSignal.Dispose();
        _pipe?.Dispose();
        return ValueTask.CompletedTask;
    }
}
