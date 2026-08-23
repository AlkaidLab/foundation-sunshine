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
    private readonly bool _authoredHapticsAvailable;
    private readonly bool _genshinCompatibilityAvailable;
    private readonly Channel<Protocol.Message> _controlOutgoing;
    private readonly Channel<Protocol.Message> _realtimeOutgoing;
    private readonly SemaphoreSlim _outgoingSignal = new(0, 1);
    private NamedPipeServerStream? _pipe;
    private CancellationTokenSource? _sessionCancellation;

    internal SidecarServer(string pipeName)
    {
        _pipeName = pipeName;
        var compositeProfileValidated = LoadPatchedProfiles();
        _context.LoadDefaultProfiles();
        _authoredHapticsAvailable = compositeProfileValidated &&
                                    _context.GetProfile(DualSenseHapticsAudio.CompositeProfileId) is not null &&
                                    HMContext.IsUsbipBackendAvailable;
        _genshinCompatibilityAvailable = _authoredHapticsAvailable &&
                                         _context.GetProfile(
                                             DualSenseHapticsAudio.GenshinCompatibilityProfileId) is not null;
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
            if (!OwnerVerification.ClientIsElevated(pipe))
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
                    DisposeControllers();
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
                            Protocol.UInt32((uint)(Protocol.Capability.Hid |
                                                   Protocol.Capability.Output |
                                                   Protocol.Capability.Touchpad |
                                                   Protocol.Capability.Motion |
                                                   Protocol.Capability.Battery |
                                                   Protocol.Capability.AdaptiveTriggers |
                                                   Protocol.Capability.AudioPolicyViolation |
                                                   (_genshinCompatibilityAvailable
                                                       ? Protocol.Capability.GenshinCompatibilityIdentity
                                                       : 0) |
                                                   (_authoredHapticsAvailable
                                                       ? Protocol.Capability.AudioFourChannel |
                                                         Protocol.Capability.AuthoredHapticsPcm
                                                       : 0)))));
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
        // id:u8, controller:u8, profile:u8 (0 HID, 1 composite), flags:u8
        if (payload.Length != 4)
            throw new InvalidDataException("Invalid attach payload");
        var deviceId = payload[0];
        if (_controllers.ContainsKey(deviceId))
            throw new InvalidOperationException("The requested DS5 device already exists");

        var profileId = SelectProfileId(
            payload[2], (Protocol.AttachFlags)payload[3],
            _authoredHapticsAvailable, _genshinCompatibilityAvailable);
        var profile = _context.GetProfile(profileId)
                      ?? throw new InvalidOperationException($"HIDMaestro profile '{profileId}' is missing");
        if (!profile.RequiresUsbipBackend)
            _context.InstallDriver();

        if (profile.RequiresUsbipBackend)
        {
            try
            {
                // Seed a previously seen virtual interface before it becomes
                // present. This avoids even a transient default-device switch
                // on every attach after the first one.
                DefaultAudioEndpointPolicy.EnsureNeverDefault(
                    TimeSpan.Zero, includePhantom: true, out _);
            }
            catch (Exception error)
            {
                Console.Error.WriteLine($"Unable to preseed the DualSense audio endpoint policy: {error.Message}");
            }
        }

        var controller = _context.CreateController(profile);
        if (profile.RequiresUsbipBackend)
            controller = ApplyDefaultAudioEndpointPolicy(controller, profile);
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
                   (_genshinCompatibilityAvailable
                       ? Protocol.Capability.GenshinCompatibilityIdentity
                       : 0) |
                   (session.HasAudio
                       ? Protocol.Capability.AudioFourChannel | Protocol.Capability.AuthoredHapticsPcm
                       : 0)));
        Emit(new Protocol.Message(Protocol.MessageType.AttachReply, requestId, reply));
        // Queue the successful attach reply before monitoring starts. If the
        // endpoint is already default, Core can finish attaching and enter its
        // normal one-shot recovery path before we close this composite session.
        session.StartDefaultAudioEndpointGuard();
    }

    internal static string SelectProfileId(
        byte profileMode, Protocol.AttachFlags flags,
        bool authoredHapticsAvailable, bool genshinCompatibilityAvailable)
    {
        if ((flags & ~Protocol.AttachFlags.GenshinCompatibilityIdentity) != 0)
            throw new InvalidDataException("Unsupported DS5 attach flags");
        var genshinCompatibility = flags.HasFlag(
            Protocol.AttachFlags.GenshinCompatibilityIdentity);
        if (genshinCompatibility && profileMode != 1)
            throw new InvalidDataException("Genshin compatibility requires the composite profile");

        return profileMode switch
        {
            0 => "dualsense",
            1 when genshinCompatibility && genshinCompatibilityAvailable =>
                DualSenseHapticsAudio.GenshinCompatibilityProfileId,
            1 when genshinCompatibility =>
                throw new InvalidOperationException("Genshin compatibility identity is unavailable"),
            1 when authoredHapticsAvailable => DualSenseHapticsAudio.CompositeProfileId,
            1 => throw new InvalidOperationException("Validated DualSense four-channel audio is unavailable"),
            _ => throw new InvalidDataException("Unsupported DS5 profile mode"),
        };
    }

    private HMController ApplyDefaultAudioEndpointPolicy(HMController controller, HMProfile profile)
    {
        const int recreationLimit = 2;
        for (var recreation = 0; recreation <= recreationLimit; ++recreation)
        {
            bool changed;
            try
            {
                if (!DefaultAudioEndpointPolicy.EnsureNeverDefault(
                        TimeSpan.FromSeconds(3), includePhantom: false, out changed))
                {
                    Console.Error.WriteLine(
                        "Unable to find the virtual DualSense audio interface; runtime default-device guard remains active");
                    return controller;
                }
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(
                    $"Unable to apply the DualSense audio endpoint policy: {error.Message}; " +
                    "runtime default-device guard remains active");
                return controller;
            }

            if (!changed)
            {
                ApplySpeakerConfiguration();
                return controller;
            }

            // AudioEndpointBuilder consumes the EP properties when it creates
            // the MMDevice endpoint. Once disposal begins, creation failures
            // must propagate rather than returning a disposed controller.
            controller.Dispose();
            controller = _context.CreateController(profile);
        }

        Console.Error.WriteLine(
            "DualSense audio endpoint identity did not stabilize after policy provisioning; " +
            "runtime default-device guard remains active");
        ApplySpeakerConfiguration();
        return controller;
    }

    private static void ApplySpeakerConfiguration()
    {
        try
        {
            if (!DualSenseSpeakerConfiguration.Ensure(TimeSpan.FromSeconds(3), out var changed))
            {
                Console.Error.WriteLine(
                    "Unable to find the virtual DualSense render endpoint; " +
                    "complete its quadraphonic speaker configuration manually");
            }
            else
                Console.Error.WriteLine(changed
                    ? "Configured the virtual DualSense render endpoint as quadraphonic"
                    : "Reapplied the virtual DualSense quadraphonic speaker configuration");
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(
                $"Unable to configure the virtual DualSense speakers: {error.Message}; " +
                "complete the quadraphonic speaker setup manually");
        }
    }

    private void Detach(uint requestId, ReadOnlySpan<byte> payload)
    {
        if (payload.Length != 1)
            throw new InvalidDataException("Invalid detach payload");
        if (_controllers.Remove(payload[0], out var controller))
            controller.Dispose();
        Emit(new Protocol.Message(Protocol.MessageType.DetachReply, requestId, new[] { payload[0] }));
    }

    private bool LoadPatchedProfiles()
    {
        // Upstream v1.6.2 USB DualSense profiles leave extendedReport unarmed,
        // so the vendor-blob encoder never runs and the Sony tail of report
        // 0x01 (touch fingers at bytes 33/37, rolling counter, sensors,
        // battery) idles at 0x00. Windows and raw HID consumers decode byte
        // 33 == 0x00 as a touch contact that is permanently down at (0,0),
        // which the PTP stack turns into a held drag (menus auto-focus and
        // inertia-scroll). Register our alwaysArmed copies before the stock
        // catalog: profile loads skip duplicate IDs, so the first
        // registration wins.
        try
        {
            var assembly = typeof(SidecarServer).Assembly;
            var directory = Path.Combine(Path.GetTempPath(), "sunshine-ds5-profiles");
            Directory.CreateDirectory(directory);
            foreach (var id in new[] { "dualsense", "dualsense-composite" })
            {
                var resourceName = $"Sunshine.Ds5Sidecar.profiles.{id}.json";
                using var stream = assembly.GetManifestResourceStream(resourceName)
                    ?? throw new InvalidOperationException($"Embedded profile '{resourceName}' is missing");
                using var memory = new MemoryStream();
                stream.CopyTo(memory);
                var profileBytes = memory.ToArray();
                if (id == DualSenseHapticsAudio.CompositeProfileId)
                    profileBytes = DualSenseHapticsAudio.CreateRuntimeCompositeProfile(profileBytes);
                File.WriteAllBytes(Path.Combine(directory, id + ".json"), profileBytes);
                if (id == DualSenseHapticsAudio.CompositeProfileId)
                {
                    var compatibilityProfile =
                        DualSenseHapticsAudio.CreateGenshinCompatibilityProfile(profileBytes);
                    File.WriteAllBytes(
                        Path.Combine(directory, DualSenseHapticsAudio.GenshinCompatibilityProfileId + ".json"),
                        compatibilityProfile);
                }
            }
            if (_context.LoadProfilesFromDirectory(directory) < 3)
                throw new InvalidOperationException("Patched DualSense profiles did not fully register");
            return true;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Unable to load patched DualSense profiles: {error.Message}");
            return false;
        }
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
