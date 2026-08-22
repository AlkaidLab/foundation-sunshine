using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text.Json;

namespace Sunshine.Ds5Sidecar;

internal static class ProtocolSelfTest
{
    internal static async Task<int> RunAsync(bool composite, string? resultPath, string? audioWriterPath)
    {
        RunDeterministicChecks();
        VerifyAdaptiveTriggerEncoding();
        var pipeName = $"sunshine-ds5-self-test-{Environment.ProcessId}-{Guid.NewGuid():N}";
        using var stopping = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        await using var server = new SidecarServer(pipeName);
        var serverTask = server.RunAsync(stopping.Token);

        await using var client = new NamedPipeClientStream(
            ".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.WriteThrough);
        await client.ConnectAsync(10_000, stopping.Token);

        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Hello, 1, new byte[4]), stopping.Token);
        var hello = await ReceiveAsync(client, stopping.Token);
        Require(hello.Type == Protocol.MessageType.HelloReply && hello.RequestId == 1 && hello.Payload.Length == 4,
            "hello reply");
        var helloCapabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(hello.Payload);
        Require(helloCapabilities.HasFlag(Protocol.Capability.Hid), "hello HID capability");
        Require(helloCapabilities.HasFlag(Protocol.Capability.AdaptiveTriggers), "hello adaptive trigger capability");
        Require(helloCapabilities.HasFlag(Protocol.Capability.GenshinCompatibilityIdentity),
            "hello Genshin compatibility identity capability");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Attach, 2, new byte[] { 0, 0, composite ? (byte)1 : (byte)0, 0 }), stopping.Token);
        var attach = await ReceiveAsync(client, stopping.Token);
        if (attach.Type == Protocol.MessageType.Error)
            throw new InvalidOperationException(DecodeError(attach.Payload));
        Require(attach.Type == Protocol.MessageType.AttachReply && attach.RequestId == 2 && attach.Payload.Length == 8,
            "attach reply");
        var capabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(attach.Payload.AsSpan(4));
        Require(capabilities.HasFlag(Protocol.Capability.Hid), "HID capability");
        Require(capabilities.HasFlag(Protocol.Capability.AdaptiveTriggers), "adaptive trigger capability");
        Require(!composite || capabilities.HasFlag(Protocol.Capability.AudioFourChannel),
            "composite four-channel audio capability");

        var input = new byte[20];
        input[0] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(input.AsSpan(4), 0x1000 | 0x0010); // Cross + Start
        input[8] = 64;
        input[9] = 128;
        BinaryPrimitives.WriteInt16LittleEndian(input.AsSpan(12), -1234);
        BinaryPrimitives.WriteInt16LittleEndian(input.AsSpan(14), 2345);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.InputState, 0, input), stopping.Token);

        var touch = new byte[20];
        touch[0] = 0;
        touch[1] = 1;
        BinaryPrimitives.WriteUInt32LittleEndian(touch.AsSpan(4), 42);
        WriteFloat(touch.AsSpan(8), 0.25f);
        WriteFloat(touch.AsSpan(12), 0.75f);
        WriteFloat(touch.AsSpan(16), 1.0f);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);
        touch[1] = 2;
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);
        touch[1] = 5; // LI_TOUCH_EVENT_BUTTON_ONLY must not mutate contact state or fail.
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);

        var motion = new byte[16];
        motion[0] = 0;
        motion[1] = 1;
        WriteFloat(motion.AsSpan(4), 0.0f);
        WriteFloat(motion.AsSpan(8), 9.80665f);
        WriteFloat(motion.AsSpan(12), 0.0f);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Motion, 0, motion), stopping.Token);

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Battery, 0, new byte[] { 0, 3, 80, 0 }), stopping.Token);

        var capturedHapticsBytes = 0;
        if (!string.IsNullOrWhiteSpace(audioWriterPath))
        {
            Require(composite, "audio writer requires composite profile");
            Require(File.Exists(audioWriterPath), "audio writer executable");
            await Task.Delay(500, stopping.Token);
            using var writer = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = audioWriterPath,
                WorkingDirectory = Path.GetDirectoryName(audioWriterPath)!,
                UseShellExecute = false,
                CreateNoWindow = true,
            }) ?? throw new InvalidOperationException("Unable to launch the haptics audio writer");
            while (capturedHapticsBytes == 0)
            {
                var haptics = await ReceiveUntilTypeAsync(client, Protocol.MessageType.HapticsPcm, stopping.Token);
                Require(haptics.Payload.Length >= 24 && haptics.Payload[3] == 2 && haptics.Payload[6] == 16,
                    "haptics PCM format");
                var frameCount = BinaryPrimitives.ReadUInt16LittleEndian(haptics.Payload.AsSpan(4));
                Require(BinaryPrimitives.ReadUInt32LittleEndian(haptics.Payload.AsSpan(20)) == 48000 &&
                        haptics.Payload.Length == 24 + frameCount * 4,
                    "haptics PCM size");
                var energy = 0L;
                for (var i = 24; i + 1 < haptics.Payload.Length; i += 2)
                    energy += Math.Abs((int)BinaryPrimitives.ReadInt16LittleEndian(haptics.Payload.AsSpan(i, 2)));
                if (energy != 0)
                    capturedHapticsBytes = haptics.Payload.Length - 24;
            }
            await writer.WaitForExitAsync(stopping.Token);
            Require(writer.ExitCode == 0, "audio writer exit code");
        }

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Detach, 3, new byte[] { 0 }), stopping.Token);
        var detach = await ReceiveUntilAsync(client, Protocol.MessageType.DetachReply, 3, stopping.Token);
        Require(detach.Payload.Length == 1 && detach.Payload[0] == 0, "detach reply");

        // Reattach and intentionally drop the owner pipe without DETACH. Completion
        // of serverTask proves the EOF path disposed the still-attached controller.
        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Attach, 4, new byte[] { 0, 0, composite ? (byte)1 : (byte)0, 0 }), stopping.Token);
        var ownerCleanupAttach = await ReceiveUntilAsync(
            client, Protocol.MessageType.AttachReply, 4, stopping.Token);
        Require(ownerCleanupAttach.Payload.Length == 8, "owner cleanup attach reply");

        client.Dispose();
        await serverTask.WaitAsync(TimeSpan.FromSeconds(10));

        var result = JsonSerializer.Serialize(new
        {
            protocol = Protocol.Version,
            profile = composite ? "dualsense-composite" : "dualsense",
            hello = true,
            attached = true,
            four_channel_audio = capabilities.HasFlag(Protocol.Capability.AudioFourChannel),
            input = true,
            touch = true,
            motion = true,
            battery = true,
            adaptive_triggers = true,
            haptics_pcm = capturedHapticsBytes != 0,
            haptics_bytes = capturedHapticsBytes,
            detached = true,
            owner_disconnect_cleanup = true,
            cleanup = true,
        });
        Console.WriteLine(result);
        if (!string.IsNullOrWhiteSpace(resultPath))
            await File.WriteAllTextAsync(resultPath, result, stopping.Token);
        return 0;
    }

    internal static void RunDeterministicChecks()
    {
        VerifyBundledCompositeProfile();
        VerifyProfileSelection();
        VerifyHapticsChannelIsolation();
        VerifyDefaultAudioEndpointClassification();
        VerifyDefaultAudioEndpointPolicy();
        VerifyControllerStateSubmissionPolicy();
    }

    private static void VerifyProfileSelection()
    {
        Require(SidecarServer.SelectProfileId(
                1, Protocol.AttachFlags.GenshinCompatibilityIdentity, true, true) ==
                DualSenseHapticsAudio.GenshinCompatibilityProfileId,
            "Genshin compatibility attach profile selection");
        Require(SidecarServer.SelectProfileId(
                1, Protocol.AttachFlags.None, true, true) ==
                DualSenseHapticsAudio.CompositeProfileId,
            "standard composite attach profile selection");
        try
        {
            SidecarServer.SelectProfileId(
                0, Protocol.AttachFlags.GenshinCompatibilityIdentity, true, true);
            Require(false, "Genshin compatibility HID attach rejection");
        }
        catch (InvalidDataException)
        {
            // Expected.
        }
    }

    private static void VerifyControllerStateSubmissionPolicy()
    {
        var policy = new ControllerStateSubmissionPolicy();
        Require(!policy.ObserveInput(0, true), "idle controller state coalescing");
        Require(policy.ObserveInput(0, false), "analog activation boundary");
        Require(!policy.ObserveInput(0, false), "continuous analog state coalescing");
        Require(policy.ObserveInput(0, true), "analog neutral boundary");
        Require(policy.ObserveInput(0x1000, true), "button press boundary");
        Require(policy.ObserveInput(0, true), "button release boundary");
    }

    private static void VerifyDefaultAudioEndpointClassification()
    {
        Require(Enum.GetUnderlyingType(typeof(DefaultAudioEndpointGuard.AudioRole)) == typeof(int),
            "default audio role COM width");
        var virtualDualSense = new[]
        {
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"SWD\MMDEVAPI\{0.0.0.00000000}.fixture", Array.Empty<string>()),
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"USB\VID_054C&PID_0CE6&MI_00\fixture",
                new[] { @"USB\VID_054C&PID_0CE6&MI_00" }),
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"ROOT\USB\0000", new[] { @"ROOT\HIDMAESTRO_UDE" }),
        };
        Require(DefaultAudioEndpointGuard.IsVirtualDualSenseChain(virtualDualSense),
            "virtual HIDMaestro DualSense endpoint classification");

        Require(!DefaultAudioEndpointGuard.IsVirtualDualSenseChain(virtualDualSense[..2]),
            "physical DualSense endpoint exclusion");
        Require(!DefaultAudioEndpointGuard.IsVirtualDualSenseChain(new[]
        {
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"ROOT\USB\0000", new[] { @"ROOT\HIDMAESTRO_UDE" }),
        }), "unrelated HIDMaestro endpoint exclusion");
    }

    private static void VerifyDefaultAudioEndpointPolicy()
    {
        Require(DefaultAudioEndpointPolicy.NeedsUpdate(null, null),
            "missing default audio endpoint policy");
        Require(DefaultAudioEndpointPolicy.NeedsUpdate(
                "{00000000-0000-0000-0000-000000000000}", 0x00000101),
            "partial default audio endpoint policy");
        Require(!DefaultAudioEndpointPolicy.NeedsUpdate(
                "{00000000-0000-0000-0000-000000000000}", 0x00000307),
            "complete default audio endpoint policy");
        var quadraphonic = DualSenseSpeakerConfiguration.CreateQuadraphonicFormat();
        Require(DualSenseSpeakerConfiguration.HasValidInteropLayout(),
            "Core Audio interop ABI");
        Require(DualSenseSpeakerConfiguration.IsQuadraphonic(quadraphonic),
            "quadraphonic Core Audio speaker configuration");
        quadraphonic.ChannelMask = 0x00000003;
        Require(!DualSenseSpeakerConfiguration.IsQuadraphonic(quadraphonic),
            "stereo Core Audio speaker configuration rejection");
    }

    private static void VerifyBundledCompositeProfile()
    {
        using var stream = typeof(ProtocolSelfTest).Assembly.GetManifestResourceStream(
            "Sunshine.Ds5Sidecar.profiles.dualsense-composite.json")
            ?? throw new InvalidOperationException("Bundled composite profile is missing");
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        var profileJson = DualSenseHapticsAudio.CreateRuntimeCompositeProfile(memory.ToArray());
        DualSenseHapticsAudio.ValidateCompositeProfile(profileJson);
        var compatibilityProfile = DualSenseHapticsAudio.CreateGenshinCompatibilityProfile(profileJson);
        using (var compatibilityDocument = JsonDocument.Parse(compatibilityProfile))
        {
            var root = compatibilityDocument.RootElement;
            Require(root.GetProperty("id").GetString() ==
                    DualSenseHapticsAudio.GenshinCompatibilityProfileId,
                "Genshin compatibility profile id");
            Require(root.GetProperty("productString").GetString() ==
                    DualSenseHapticsAudio.GenshinCompatibilityProductString,
                "Genshin compatibility product string");
            Require(root.GetProperty("vid").GetString() == "0x054C" &&
                    root.GetProperty("pid").GetString() == "0x0CE6",
                "Genshin compatibility profile preserves Sony identity");
        }

        var profileText = System.Text.Encoding.UTF8.GetString(profileJson);
        RequireProfileRejected(profileText.Replace("\"channels\":4", "\"channels\":2", StringComparison.Ordinal),
            "stereo composite profile rejection");
        RequireProfileRejected(profileText.Replace(
                "\"hapticLeft\",\"hapticRight\"",
                "\"hapticRight\",\"hapticLeft\"",
                StringComparison.Ordinal),
            "swapped haptics role rejection");
        RequireProfileRejected(profileText.Replace(
                "\"volumeCurRaw\":0",
                "\"volumeCurRaw\":-25600",
                StringComparison.Ordinal),
            "muted speaker control rejection");
    }

    private static void RequireProfileRejected(string profileJson, string operation)
    {
        try
        {
            DualSenseHapticsAudio.ValidateCompositeProfile(System.Text.Encoding.UTF8.GetBytes(profileJson));
            Require(false, operation);
        }
        catch (InvalidDataException)
        {
            // Expected.
        }
    }

    private static void VerifyHapticsChannelIsolation()
    {
        var frames = new byte[DualSenseHapticsAudio.InputFrameBytes * 2];
        WriteSample(frames, 0, 1234);
        WriteSample(frames, 1, -2345);
        WriteSample(frames, 4, short.MaxValue);
        WriteSample(frames, 5, short.MinValue);
        var speakerOnly = DualSenseHapticsAudio.Extract(frames);
        Require(speakerOnly.AsSpan().IndexOfAnyExcept((byte)0) == -1,
            "speaker channels cannot leak into haptics");

        WriteSample(frames, 2, 3456);
        WriteSample(frames, 3, -4567);
        WriteSample(frames, 6, 5678);
        WriteSample(frames, 7, -6789);
        var haptics = DualSenseHapticsAudio.Extract(frames);
        Require(BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(0, 2)) == 3456 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(2, 2)) == -4567 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(4, 2)) == 5678 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(6, 2)) == -6789,
            "haptics channels preserve exact samples");

        try
        {
            DualSenseHapticsAudio.Extract(frames.AsSpan(0, frames.Length - 1));
            Require(false, "incomplete four-channel frame rejection");
        }
        catch (InvalidDataException)
        {
            // Expected: arbitrary callback fragmentation is reassembled by
            // ControllerSession before complete frames reach the extractor.
        }
    }

    private static void WriteSample(Span<byte> frames, int sample, short value) =>
        BinaryPrimitives.WriteInt16LittleEndian(frames.Slice(sample * 2, 2), value);

    private static async Task SendAsync(Stream stream, Protocol.Message message, CancellationToken cancellationToken)
    {
        var frame = Protocol.Encode(message);
        await stream.WriteAsync(frame, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    private static async Task<Protocol.Message> ReceiveAsync(Stream stream, CancellationToken cancellationToken)
    {
        var headerBytes = new byte[Protocol.HeaderSize];
        await stream.ReadExactlyAsync(headerBytes, cancellationToken);
        var header = Protocol.DecodeHeader(headerBytes);
        var payload = new byte[header.PayloadLength];
        if (payload.Length != 0)
            await stream.ReadExactlyAsync(payload, cancellationToken);
        return new Protocol.Message(header.Type, header.RequestId, payload);
    }

    private static async Task<Protocol.Message> ReceiveUntilAsync(
        Stream stream, Protocol.MessageType type, uint requestId, CancellationToken cancellationToken)
    {
        while (true)
        {
            var message = await ReceiveAsync(stream, cancellationToken);
            if (message.Type == Protocol.MessageType.Error && message.RequestId == requestId)
                throw new InvalidOperationException(DecodeError(message.Payload));
            if (message.Type == type && message.RequestId == requestId)
                return message;
        }
    }

    private static async Task<Protocol.Message> ReceiveUntilTypeAsync(
        Stream stream, Protocol.MessageType type, CancellationToken cancellationToken)
    {
        while (true)
        {
            var message = await ReceiveAsync(stream, cancellationToken);
            if (message.Type == Protocol.MessageType.Error)
                throw new InvalidOperationException(DecodeError(message.Payload));
            if (message.Type == type)
                return message;
        }
    }

    private static string DecodeError(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 8) return "Malformed sidecar error";
        var length = Math.Min(BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)), (uint)(payload.Length - 8));
        return System.Text.Encoding.UTF8.GetString(payload.Slice(8, (int)length));
    }

    private static void WriteFloat(Span<byte> destination, float value) =>
        BinaryPrimitives.WriteInt32LittleEndian(destination, BitConverter.SingleToInt32Bits(value));

    private static void VerifyAdaptiveTriggerEncoding()
    {
        var state = new AdaptiveTriggerState();
        var left = Enumerable.Range(0x20, AdaptiveTriggerState.EffectSize).Select(value => (byte)value).ToArray();
        var right = Enumerable.Range(0x40, AdaptiveTriggerState.EffectSize).Select(value => (byte)value).ToArray();

        Require(state.TryUpdate(new Dictionary<string, object> { ["leftTriggerEffect"] = left },
                                3, 2, out var leftMessage),
            "left adaptive trigger update");
        Require(leftMessage.Type == Protocol.MessageType.AdaptiveTriggers &&
                leftMessage.Payload.Length == 26 &&
                leftMessage.Payload[0] == 3 && leftMessage.Payload[1] == 2 &&
                leftMessage.Payload[2] == AdaptiveTriggerState.LeftFlag &&
                leftMessage.Payload[3] == left[0] && leftMessage.Payload[4] == 0 &&
                leftMessage.Payload.AsSpan(6, 10).SequenceEqual(left.AsSpan(1, 10)),
            "left adaptive trigger encoding");

        Require(!state.TryUpdate(new Dictionary<string, object> { ["leftTriggerEffect"] = left },
                                 3, 2, out _),
            "adaptive trigger duplicate suppression");

        Require(state.TryUpdate(new Dictionary<string, object> { ["rightTriggerEffect"] = right },
                                3, 2, out var rightMessage) &&
                rightMessage.Payload[2] == AdaptiveTriggerState.RightFlag &&
                rightMessage.Payload[3] == left[0] && rightMessage.Payload[4] == right[0] &&
                rightMessage.Payload.AsSpan(16, 10).SequenceEqual(right.AsSpan(1, 10)),
            "right adaptive trigger encoding");

        Require(state.TryReset(3, 2, out var resetMessage) &&
                resetMessage.Payload[2] == (AdaptiveTriggerState.LeftFlag | AdaptiveTriggerState.RightFlag) &&
                resetMessage.Payload.AsSpan(3).IndexOfAnyExcept((byte)0) == -1,
            "adaptive trigger reset");
        Require(!state.TryReset(3, 2, out _), "adaptive trigger reset duplicate suppression");
    }

    private static void Require(bool condition, string operation)
    {
        if (!condition) throw new InvalidOperationException($"Protocol self-test failed at {operation}");
    }
}
