using System.Buffers.Binary;
using System.Reflection;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal sealed class DeviceRegistry : IDisposable
{
    internal const Protocol.Capability BaseCapabilities =
        Protocol.Capability.Hid |
        Protocol.Capability.Output |
        Protocol.Capability.Touchpad |
        Protocol.Capability.Motion |
        Protocol.Capability.Battery |
        Protocol.Capability.AdaptiveTriggers |
        Protocol.Capability.AudioPolicyViolation;

    private readonly HMContext _context = new();
    private readonly Dictionary<byte, ControllerSession> _controllers = new();
    private readonly Action<Protocol.Message> _emit;
    private readonly bool _authoredHapticsAvailable;
    private readonly bool _genshinCompatibilityAvailable;
    private readonly bool _microphonePrototypeAvailable;
    private VirtualMicrophoneSession? _microphone;
    private uint _nextMicrophoneGeneration = 1;

    internal DeviceRegistry(Action<Protocol.Message> emit, bool enableCompositeMicrophonePrototype = false)
    {
        _emit = emit;
        var compositeProfileValidated = LoadPatchedProfiles();
        _context.LoadDefaultProfiles();
        _authoredHapticsAvailable = compositeProfileValidated &&
                                    _context.GetProfile(DualSenseHapticsAudio.CompositeProfileId) is not null &&
                                    HMContext.IsUsbipBackendAvailable;
        _genshinCompatibilityAvailable = _authoredHapticsAvailable &&
                                         _context.GetProfile(
                                             DualSenseHapticsAudio.GenshinCompatibilityProfileId) is not null;
        _microphonePrototypeAvailable = enableCompositeMicrophonePrototype &&
                                        _authoredHapticsAvailable &&
                                        HasPinnedMicrophoneRuntime;
    }

    internal Protocol.Capability Capabilities =>
        BaseCapabilities |
        (_genshinCompatibilityAvailable
            ? Protocol.Capability.GenshinCompatibilityIdentity
            : 0) |
        (_authoredHapticsAvailable
            ? Protocol.Capability.AudioFourChannel | Protocol.Capability.AuthoredHapticsPcm
            : 0) |
        (_microphonePrototypeAvailable
            ? Protocol.Capability.VirtualMicrophone |
              Protocol.Capability.PersistentDeviceHost |
              Protocol.Capability.MicrophoneStatus
            : 0);

    internal static bool HasPinnedMicrophoneRuntime
    {
        get
        {
            var text = typeof(HMContext).Assembly
                .GetCustomAttribute<AssemblyFileVersionAttribute>()?.Version;
            return Version.TryParse(text, out var version) && version == new Version(1, 12, 0, 0);
        }
    }

    /// <summary>Creates a client-owned controller and configures its own render endpoint before replying.</summary>
    internal void Attach(uint requestId, ReadOnlySpan<byte> payload)
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
                DefaultAudioEndpointPolicy.EnsureNeverDefault(
                    TimeSpan.Zero, includePhantom: true, out _);
            }
            catch (Exception error)
            {
                Console.Error.WriteLine($"Unable to preseed the DualSense audio endpoint policy: {error.Message}");
            }
        }

        var (controller, endpointId) = CreateController(profile);
        ControllerSession session;
        try
        {
            session = new ControllerSession(deviceId, payload[1], controller, profile, _emit, endpointId);
        }
        catch
        {
            controller.Dispose();
            if (endpointId is not null)
                DefaultAudioEndpointGuard.WaitForRenderEndpointRemoval(endpointId, TimeSpan.FromSeconds(3));
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
        _emit(new Protocol.Message(Protocol.MessageType.AttachReply, requestId, reply));
        session.StartDefaultAudioEndpointGuard();
    }

    /// <summary>Acknowledges detach after the owned session and render endpoint have been released.</summary>
    internal void Detach(uint requestId, ReadOnlySpan<byte> payload)
    {
        if (payload.Length != 1)
            throw new InvalidDataException("Invalid detach payload");
        if (_controllers.Remove(payload[0], out var controller))
            controller.Dispose();
        _emit(new Protocol.Message(Protocol.MessageType.DetachReply, requestId, new[] { payload[0] }));
    }

    /// <summary>Resolves the payload's device ID only within this registry's attached sessions.</summary>
    internal ControllerSession GetController(ReadOnlySpan<byte> payload)
    {
        if (payload.IsEmpty || !_controllers.TryGetValue(payload[0], out var controller))
            throw new InvalidOperationException("The requested DS5 device is not attached");
        return controller;
    }

    /// <summary>Creates the composite microphone with the same endpoint ownership and policy guarantees.</summary>
    internal void CreateMicrophone(uint requestId, ReadOnlySpan<byte> payload)
    {
        var request = Protocol.DecodeMicCreate(payload);
        if (request.SampleRateHz != Protocol.MicrophoneSampleRateHz ||
            request.Channels != Protocol.MicrophoneChannels ||
            request.BitsPerSample != Protocol.MicrophoneBitsPerSample || request.Flags != 0)
        {
            EmitMicCreateReply(requestId, Protocol.MicrophoneResult.InvalidFormat, 0);
            return;
        }
        if (!_microphonePrototypeAvailable)
        {
            EmitMicCreateReply(requestId, Protocol.MicrophoneResult.TransportUnavailable, 0);
            return;
        }
        if (_microphone is not null)
        {
            EmitMicCreateReply(requestId, Protocol.MicrophoneResult.Success, _microphone.Generation);
            _microphone.PublishStatus();
            return;
        }

        HMController? controller = null;
        string? endpointId = null;
        try
        {
            var profile = _context.GetProfile(DualSenseHapticsAudio.CompositeProfileId)
                          ?? throw new InvalidOperationException("Composite microphone prototype profile is missing");
            (controller, endpointId) = CreateController(profile);
            var generation = _nextMicrophoneGeneration;
            _nextMicrophoneGeneration = generation == uint.MaxValue ? 1 : generation + 1;
            _microphone = new VirtualMicrophoneSession(controller, generation, _emit, endpointId);
            controller = null; // Session owns the controller after successful construction.
            endpointId = null;
            EmitMicCreateReply(requestId, Protocol.MicrophoneResult.Success, generation);
            _microphone.PublishStatus();
        }
        catch (Exception error)
        {
            controller?.Dispose();
            if (endpointId is not null)
                DefaultAudioEndpointGuard.WaitForRenderEndpointRemoval(endpointId, TimeSpan.FromSeconds(3));
            Console.Error.WriteLine($"Unable to create the composite microphone prototype: {error.Message}");
            EmitMicCreateReply(requestId, Protocol.MicrophoneResult.DeviceCreationFailed, 0);
        }
    }

    /// <summary>Decodes generation-tagged PCM and queues it only while a microphone session exists.</summary>
    internal void SubmitMicrophonePcm(byte[] payload)
    {
        var packet = Protocol.DecodeMicPcm(payload);
        if (_microphone is null)
            return;
        _microphone.Enqueue(packet);
    }

    /// <summary>Flushes the current generation or reports a missing microphone without creating one.</summary>
    internal void FlushMicrophone(uint requestId, ReadOnlySpan<byte> payload)
    {
        if (!payload.IsEmpty)
            throw new InvalidDataException("Invalid microphone flush payload");
        if (_microphone is null)
        {
            _emit(new Protocol.Message(
                Protocol.MessageType.MicFlushReply, requestId,
                Protocol.EncodeMicOperationReply(Protocol.MicrophoneResult.DeviceNotCreated, 0)));
            return;
        }
        _microphone.Flush();
        _emit(new Protocol.Message(
            Protocol.MessageType.MicFlushReply, requestId,
            Protocol.EncodeMicOperationReply(
                Protocol.MicrophoneResult.Success, _microphone.Generation)));
    }

    /// <summary>Replies only after the microphone device and its owned render endpoint finish releasing.</summary>
    internal void DestroyMicrophone(uint requestId, ReadOnlySpan<byte> payload)
    {
        if (!payload.IsEmpty)
            throw new InvalidDataException("Invalid microphone destroy payload");
        var generation = _microphone?.Generation ?? 0;
        var result = _microphone is null
            ? Protocol.MicrophoneResult.DeviceNotCreated
            : Protocol.MicrophoneResult.Success;
        var microphone = _microphone;
        _microphone = null;
        microphone?.Dispose();
        _emit(new Protocol.Message(
            Protocol.MessageType.MicDestroyReply, requestId,
            Protocol.EncodeMicOperationReply(result, generation)));
        if (result == Protocol.MicrophoneResult.Success)
        {
            _emit(new Protocol.Message(
                Protocol.MessageType.MicStatus, 0,
                Protocol.EncodeMicStatus(new Protocol.MicrophoneStatus(
                    generation, Protocol.MicrophoneState.Absent, false,
                    0, 0, 0, 0, 0))));
        }
    }

    /// <summary>Encodes the protocol result and generation without exposing implementation exceptions.</summary>
    private void EmitMicCreateReply(
        uint requestId, Protocol.MicrophoneResult result, uint generation)
    {
        _emit(new Protocol.Message(
            Protocol.MessageType.MicCreateReply, requestId,
            Protocol.EncodeMicCreateReply((int)result, generation)));
    }

    /// <summary>Rejects unsupported flags or unavailable capabilities before selecting a controller identity.</summary>
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
            // v1.12.0 leaves JSON profiles on Windows' native mapping path
            // and removes exact legacy SDK tables; no usage override is needed.
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

    /// <summary>Captures existing endpoints before creation so another controller cannot satisfy provisioning.</summary>
    private (HMController Controller, string? EndpointId) CreateController(HMProfile profile)
    {
        var previous = profile.RequiresUsbipBackend
            ? DefaultAudioEndpointGuard.GetActiveVirtualDualSenseRenderEndpoints()
                .ToHashSet(StringComparer.OrdinalIgnoreCase)
            : null;
        var controller = _context.CreateController(profile);
        return previous is null ? (controller, null) : ApplyDefaultAudioEndpointPolicy(controller, profile, previous);
    }

    /// <summary>Recreates only after the owned endpoint retires and configures the replacement by identity.</summary>
    private (HMController Controller, string? EndpointId) ApplyDefaultAudioEndpointPolicy(
        HMController controller, HMProfile profile, IReadOnlySet<string> previous)
    {
        const int recreationLimit = 2;
        try
        {
            for (var recreation = 0; ; ++recreation)
            {
                var endpointId = DefaultAudioEndpointGuard.WaitForNewRenderEndpoint(previous, TimeSpan.FromSeconds(3));
                bool changed;
                try
                {
                    if (!DefaultAudioEndpointPolicy.EnsureNeverDefault(
                            TimeSpan.FromSeconds(3), includePhantom: false, out changed))
                    {
                        Console.Error.WriteLine(
                            "Unable to find the virtual DualSense audio interface; runtime default-device guard remains active");
                        ApplySpeakerConfiguration(endpointId);
                        return (controller, endpointId);
                    }
                }
                catch (Exception error)
                {
                    Console.Error.WriteLine(
                        $"Unable to apply the DualSense audio endpoint policy: {error.Message}; " +
                        "runtime default-device guard remains active");
                    ApplySpeakerConfiguration(endpointId);
                    return (controller, endpointId);
                }
                if (!changed || recreation == recreationLimit)
                {
                    if (changed)
                        Console.Error.WriteLine(
                            "DualSense audio endpoint identity did not stabilize after policy provisioning; " +
                            "runtime default-device guard remains active");
                    ApplySpeakerConfiguration(endpointId);
                    return (controller, endpointId);
                }
                controller.Dispose();
                DefaultAudioEndpointGuard.WaitForRenderEndpointRemoval(endpointId, TimeSpan.FromSeconds(3));
                controller = _context.CreateController(profile);
            }
        }
        catch
        {
            controller.Dispose();
            throw;
        }
    }

    /// <summary>Applies speaker settings to the selected controller while preserving HID on configuration failure.</summary>
    private static void ApplySpeakerConfiguration(string endpointId)
    {
        try
        {
            DualSenseSpeakerConfiguration.Ensure(endpointId, out var changed);
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

    /// <summary>Registers pinned profiles before SDK defaults and derives the Genshin composite identity.</summary>
    private bool LoadPatchedProfiles()
    {
        // Register Sunshine's always-armed profile copies before the stock
        // catalog. HIDMaestro keeps the first profile for duplicate ids.
        try
        {
            var assembly = typeof(DeviceRegistry).Assembly;
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

    /// <summary>Attempts every owned device release before reporting the first cleanup error.</summary>
    internal void ReleaseAllDevices()
    {
        Exception? cleanupError = null;
        try
        {
            _microphone?.Dispose();
        }
        catch (Exception error)
        {
            cleanupError = error;
        }
        finally
        {
            _microphone = null;
        }
        foreach (var controller in _controllers.Values)
        {
            try
            {
                controller.Dispose();
            }
            catch (Exception error)
            {
                cleanupError ??= error;
            }
        }
        _controllers.Clear();
        if (cleanupError is not null)
            throw new InvalidOperationException("One or more virtual devices failed to dispose", cleanupError);
    }

    /// <summary>Always releases the SDK context, including when one of its sessions fails to dispose.</summary>
    public void Dispose()
    {
        try { ReleaseAllDevices(); }
        finally { _context.Dispose(); }
    }
}
