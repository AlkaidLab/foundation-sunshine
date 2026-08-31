using System.Buffers.Binary;

namespace Sunshine.Ds5Sidecar;

internal static class Protocol
{
    internal const uint Magic = 0x35534453; // "SDS5" on the wire
    internal const ushort Version = 1;
    internal const int HeaderSize = 16;
    internal const int MaxPayloadSize = 1024 * 1024;
    internal const uint MicrophoneSampleRateHz = 48_000;
    internal const byte MicrophoneChannels = 1;
    internal const byte MicrophoneBitsPerSample = 16;
    internal const int MicCreatePayloadSize = 8;
    internal const int MicCreateReplyPayloadSize = 16;
    internal const int MicOperationReplyPayloadSize = 8;
    internal const int MicPcmHeaderSize = 20;
    internal const int MicStatusPayloadSize = 28;
    internal const ushort MaxMicPcmFrames = 960;

    [Flags]
    internal enum Capability : uint
    {
        Hid = 1u << 0,
        Output = 1u << 1,
        AudioFourChannel = 1u << 2,
        AuthoredHapticsPcm = 1u << 3,
        Touchpad = 1u << 4,
        Motion = 1u << 5,
        Battery = 1u << 6,
        AdaptiveTriggers = 1u << 7,
        GenshinCompatibilityIdentity = 1u << 8,
        AudioPolicyViolation = 1u << 9,
        VirtualMicrophone = 1u << 10,
        PersistentDeviceHost = 1u << 11,
        MicrophoneStatus = 1u << 12,
    }

    [Flags]
    internal enum AttachFlags : byte
    {
        None = 0,
        GenshinCompatibilityIdentity = 1 << 0,
    }

    internal enum MessageType : ushort
    {
        Hello = 1,
        HelloReply = 2,
        Attach = 3,
        AttachReply = 4,
        Detach = 5,
        DetachReply = 6,
        InputState = 7,
        Touch = 8,
        Motion = 9,
        Battery = 10,
        Shutdown = 11,
        MicCreate = 12,
        MicCreateReply = 13,
        MicPcm = 14,
        MicFlush = 15,
        MicFlushReply = 16,
        MicDestroy = 17,
        MicDestroyReply = 18,
        Status = 100,
        Rumble = 101,
        AdaptiveTriggers = 102,
        Led = 103,
        HapticsPcm = 104,
        AudioPolicyViolation = 105,
        HostStatus = 106,
        MicStatus = 107,
        Error = 255,
    }

    [Flags]
    internal enum MicPcmFlags : ushort
    {
        None = 0,
        StreamStart = 1 << 0,
        StreamEnd = 1 << 1,
        Discontinuity = 1 << 2,
        Silence = 1 << 3,
    }

    internal enum MicrophoneState : byte
    {
        Absent = 0,
        Creating = 1,
        Enumerating = 2,
        Idle = 3,
        HostCapturing = 4,
        RemoteActive = 5,
        Destroying = 6,
        DeviceFaulted = 7,
    }

    internal enum MicrophoneResult : int
    {
        Success = 0,
        InvalidFormat = -1001,
        TransportUnavailable = -1002,
        DeviceCreationFailed = -1003,
        DeviceNotCreated = -1004,
    }

    [Flags]
    internal enum HapticsFlags : byte
    {
        None = 0,
        StreamStart = 1 << 0,
        StreamEnd = 1 << 1,
        Discontinuity = 1 << 2,
    }

    internal readonly record struct Header(MessageType Type, uint PayloadLength, uint RequestId);
    internal readonly record struct Message(MessageType Type, uint RequestId, byte[] Payload);
    internal readonly record struct MicCreateRequest(
        uint SampleRateHz, byte Channels, byte BitsPerSample, ushort Flags);
    internal readonly record struct MicPcmPacket(
        uint Generation, uint Sequence, ulong CaptureTimeUs, ushort FrameCount,
        MicPcmFlags Flags, ReadOnlyMemory<byte> Pcm);
    internal readonly record struct MicrophoneStatus(
        uint Generation, MicrophoneState State, bool IsHostStreaming, uint BufferedBytes,
        uint Underruns, uint DroppedFrames, uint SubmitErrors, int LastError);

    internal static byte[] Encode(Message message)
    {
        var frame = new byte[HeaderSize + message.Payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(0, 4), Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4, 2), Version);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6, 2), (ushort)message.Type);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8, 4), (uint)message.Payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(12, 4), message.RequestId);
        message.Payload.CopyTo(frame, HeaderSize);
        return frame;
    }

    internal static Header DecodeHeader(ReadOnlySpan<byte> frame)
    {
        if (frame.Length != HeaderSize ||
            BinaryPrimitives.ReadUInt32LittleEndian(frame[..4]) != Magic ||
            BinaryPrimitives.ReadUInt16LittleEndian(frame.Slice(4, 2)) != Version)
        {
            throw new InvalidDataException("Invalid DS5 sidecar protocol header");
        }

        var payloadLength = BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(8, 4));
        if (payloadLength > MaxPayloadSize)
            throw new InvalidDataException("DS5 sidecar payload exceeds the protocol limit");

        return new Header(
            (MessageType)BinaryPrimitives.ReadUInt16LittleEndian(frame.Slice(6, 2)),
            payloadLength,
            BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(12, 4)));
    }

    internal static byte[] UInt32(uint value)
    {
        var data = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(data, value);
        return data;
    }

    internal static MicCreateRequest DecodeMicCreate(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != MicCreatePayloadSize)
            throw new InvalidDataException("Invalid microphone create payload");

        return new MicCreateRequest(
            BinaryPrimitives.ReadUInt32LittleEndian(payload[..4]),
            payload[4],
            payload[5],
            BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(6, 2)));
    }

    internal static byte[] EncodeMicCreateReply(
        int result, uint generation, uint sampleRateHz = MicrophoneSampleRateHz,
        byte channels = MicrophoneChannels, byte bitsPerSample = MicrophoneBitsPerSample)
    {
        var payload = new byte[MicCreateReplyPayloadSize];
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(0, 4), result);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), generation);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), sampleRateHz);
        payload[12] = channels;
        payload[13] = bitsPerSample;
        return payload;
    }

    internal static byte[] EncodeMicOperationReply(MicrophoneResult result, uint generation)
    {
        var payload = new byte[MicOperationReplyPayloadSize];
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(0, 4), (int)result);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), generation);
        return payload;
    }

    internal static MicPcmPacket DecodeMicPcm(byte[] payload)
    {
        if (payload.Length < MicPcmHeaderSize)
            throw new InvalidDataException("Invalid microphone PCM payload");

        var frameCount = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(16, 2));
        if (frameCount > MaxMicPcmFrames)
            throw new InvalidDataException("Microphone PCM packet exceeds 20 ms");

        int expectedLength;
        try
        {
            expectedLength = checked(MicPcmHeaderSize + frameCount * MicrophoneChannels *
                                     (MicrophoneBitsPerSample / 8));
        }
        catch (OverflowException error)
        {
            throw new InvalidDataException("Microphone PCM payload length overflow", error);
        }
        if (payload.Length != expectedLength)
            throw new InvalidDataException("Microphone PCM payload length does not match frame count");

        var flags = (MicPcmFlags)BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(18, 2));
        const MicPcmFlags knownFlags = MicPcmFlags.StreamStart | MicPcmFlags.StreamEnd |
                                       MicPcmFlags.Discontinuity | MicPcmFlags.Silence;
        if ((flags & ~knownFlags) != 0)
            throw new InvalidDataException("Unsupported microphone PCM flags");

        return new MicPcmPacket(
            BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(0, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(8, 8)),
            frameCount,
            flags,
            payload.AsMemory(MicPcmHeaderSize));
    }

    internal static byte[] EncodeMicStatus(MicrophoneStatus status)
    {
        var payload = new byte[MicStatusPayloadSize];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), status.Generation);
        payload[4] = (byte)status.State;
        payload[5] = status.IsHostStreaming ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), status.BufferedBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12, 4), status.Underruns);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), status.DroppedFrames);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), status.SubmitErrors);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(24, 4), status.LastError);
        return payload;
    }

    internal static byte[] ErrorPayload(int code, string message)
    {
        var text = System.Text.Encoding.UTF8.GetBytes(message);
        var payload = new byte[8 + text.Length];
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(0, 4), code);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), (uint)text.Length);
        text.CopyTo(payload, 8);
        return payload;
    }
}
