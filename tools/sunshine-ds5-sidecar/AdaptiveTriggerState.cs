namespace Sunshine.Ds5Sidecar;

internal sealed class AdaptiveTriggerState
{
    internal const byte RightFlag = 0x04;
    internal const byte LeftFlag = 0x08;
    internal const int EffectSize = 11;

    private readonly object _lock = new();
    private readonly byte[] _left = new byte[EffectSize];
    private readonly byte[] _right = new byte[EffectSize];

    internal bool TryUpdate(IReadOnlyDictionary<string, object> fields,
                            OutputValidFlags valid,
                            byte deviceId,
                            byte controllerNumber,
                            out Protocol.Message message)
    {
        lock (_lock)
        {
            byte flags = 0;
            // A trigger this report is not programming keeps the effect the
            // client holds; its zero bytes are not a release.
            if (valid.LeftTrigger &&
                TryGetEffect(fields, "leftTriggerEffect", out var left) &&
                !_left.AsSpan().SequenceEqual(left))
            {
                left.CopyTo(_left);
                flags |= LeftFlag;
            }

            if (valid.RightTrigger &&
                TryGetEffect(fields, "rightTriggerEffect", out var right) &&
                !_right.AsSpan().SequenceEqual(right))
            {
                right.CopyTo(_right);
                flags |= RightFlag;
            }

            if (flags == 0)
            {
                message = default;
                return false;
            }

            message = BuildMessage(deviceId, controllerNumber, flags, _left, _right);
            return true;
        }
    }

    internal bool TryReset(byte deviceId, byte controllerNumber, out Protocol.Message message)
    {
        lock (_lock)
        {
            if (!_left.AsSpan().ContainsAnyExcept((byte)0) &&
                !_right.AsSpan().ContainsAnyExcept((byte)0))
            {
                message = default;
                return false;
            }

            Array.Clear(_left);
            Array.Clear(_right);
            message = BuildMessage(deviceId, controllerNumber, LeftFlag | RightFlag, _left, _right);
            return true;
        }
    }

    private static bool TryGetEffect(IReadOnlyDictionary<string, object> fields,
                                     string name,
                                     out ReadOnlySpan<byte> effect)
    {
        if (fields.TryGetValue(name, out var item) &&
            item is byte[] bytes &&
            bytes.Length >= EffectSize)
        {
            effect = bytes.AsSpan(0, EffectSize);
            return true;
        }

        effect = default;
        return false;
    }

    private static Protocol.Message BuildMessage(byte deviceId,
                                                 byte controllerNumber,
                                                 byte flags,
                                                 ReadOnlySpan<byte> left,
                                                 ReadOnlySpan<byte> right)
    {
        // id:u8, controller:u8, flags:u8, left/right type:u8, reserved:u8,
        // left/right effect payload:10 bytes = 26 bytes.
        var payload = new byte[26];
        payload[0] = deviceId;
        payload[1] = controllerNumber;
        payload[2] = flags;
        payload[3] = left[0];
        payload[4] = right[0];
        left.Slice(1, 10).CopyTo(payload.AsSpan(6, 10));
        right.Slice(1, 10).CopyTo(payload.AsSpan(16, 10));
        return new Protocol.Message(Protocol.MessageType.AdaptiveTriggers, 0, payload);
    }
}
