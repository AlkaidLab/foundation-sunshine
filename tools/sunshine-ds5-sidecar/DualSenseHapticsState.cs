namespace Sunshine.Ds5Sidecar;

/// <summary>Reduces claimed USB output fields to an absolute haptic-control snapshot.</summary>
internal sealed class DualSenseHapticsState
{
    // mode0, mode2, known control groups, right, left, power-save, reduction, filter.
    private readonly byte[] _state = new byte[8];
    private bool _initialized;

    internal bool TryReset(out byte[] state)
    {
        state = Array.Empty<byte>();
        if (!_initialized) return false;
        _state[0] = _state[1] = _state[3] = _state[4] = 0;
        _state[5] = _state[6] = _state[7] = 0;
        _initialized = false;
        state = (byte[])_state.Clone();
        return true;
    }

    internal byte Left => (_state[0] & 1) != 0 || (_state[1] & 4) != 0 ? _state[4] : (byte)0;
    internal byte Right => (_state[0] & 1) != 0 || (_state[1] & 4) != 0 ? _state[3] : (byte)0;

    internal bool TryUpdate(ReadOnlySpan<byte> effect, out byte[] state)
    {
        state = Array.Empty<byte>();
        if (effect.Length != 47) return false;
        var previous = (byte[])_state.Clone();
        // These bits select the current actuator mode, including an explicit zero/PCM mode.
        _state[0] = (byte)(effect[0] & 3);
        _state[1] = (byte)(effect[38] & 4);
        if ((_state[0] & 1) != 0 || (_state[1] & 4) != 0)
        {
            _state[3] = effect[2];
            _state[4] = effect[3];
        }
        Merge(effect, 0x02, 9, 5, 0xff);
        Merge(effect, 0x40, 36, 6, 0xff);
        Merge(effect, 0x20, 39, 7, 0x01);
        if (_initialized && previous.AsSpan().SequenceEqual(_state)) return false;
        _initialized = true;
        state = (byte[])_state.Clone();
        return true;
    }

    private void Merge(ReadOnlySpan<byte> effect, byte valid, int source, int target, byte mask)
    {
        if ((effect[1] & valid) == 0) return;
        _state[2] |= valid;
        _state[target] = (byte)(effect[source] & mask);
    }
}
