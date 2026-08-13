using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text.Json;

namespace Sunshine.Ds5Sidecar;

internal static class ProtocolSelfTest
{
    internal static async Task<int> RunAsync(bool composite, string? resultPath, string? audioWriterPath)
    {
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

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Attach, 2, new byte[] { 0, 0, composite ? (byte)1 : (byte)0, 0 }), stopping.Token);
        var attach = await ReceiveAsync(client, stopping.Token);
        if (attach.Type == Protocol.MessageType.Error)
            throw new InvalidOperationException(DecodeError(attach.Payload));
        Require(attach.Type == Protocol.MessageType.AttachReply && attach.RequestId == 2 && attach.Payload.Length == 8,
            "attach reply");
        var capabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(attach.Payload.AsSpan(4));
        Require(capabilities.HasFlag(Protocol.Capability.Hid), "HID capability");
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
            haptics_pcm = capturedHapticsBytes != 0,
            haptics_bytes = capturedHapticsBytes,
            detached = true,
            cleanup = true,
        });
        Console.WriteLine(result);
        if (!string.IsNullOrWhiteSpace(resultPath))
            await File.WriteAllTextAsync(resultPath, result, stopping.Token);
        return 0;
    }

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

    private static void Require(bool condition, string operation)
    {
        if (!condition) throw new InvalidOperationException($"Protocol self-test failed at {operation}");
    }
}
