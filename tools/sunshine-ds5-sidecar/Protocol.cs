using System.Buffers.Binary;

namespace Sunshine.Ds5Sidecar;

internal static class Protocol
{
    internal const uint Magic = 0x35534453; // "SDS5" on the wire
    internal const ushort Version = 1;
    internal const int HeaderSize = 16;
    internal const int MaxPayloadSize = 1024 * 1024;

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
        Status = 100,
        Rumble = 101,
        AdaptiveTriggers = 102,
        Led = 103,
        HapticsPcm = 104,
        AudioPolicyViolation = 105,
        Error = 255,
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
