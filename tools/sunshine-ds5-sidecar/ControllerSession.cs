using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal sealed class ControllerSession : IDisposable
{
    private const uint DpadUp = 0x0001;
    private const uint DpadDown = 0x0002;
    private const uint DpadLeft = 0x0004;
    private const uint DpadRight = 0x0008;
    private const uint Start = 0x0010;
    private const uint Back = 0x0020;
    private const uint LeftStick = 0x0040;
    private const uint RightStick = 0x0080;
    private const uint LeftBumper = 0x0100;
    private const uint RightBumper = 0x0200;
    private const uint Guide = 0x0400;
    private const uint A = 0x1000;
    private const uint B = 0x2000;
    private const uint X = 0x4000;
    private const uint Y = 0x8000;
    private const uint Touchpad = 0x100000;
    private const uint Misc = 0x200000;
    private const int HapticsFramesPerPacket = 240;
    private static readonly TimeSpan StateCoalesceWindow = TimeSpan.FromMilliseconds(4);

    private readonly object _stateLock = new();
    private readonly object _outputLock = new();
    private readonly HMController _controller;
    private readonly HMProfile _profile;
    private readonly HMAudioOutput? _audioOutput;
    private readonly Action<Protocol.Message> _emit;
    private readonly System.Threading.Timer _stateSubmitTimer;
    private DefaultAudioEndpointGuard? _audioEndpointGuard;
    private readonly AdaptiveTriggerState _adaptiveTriggers = new();
    private readonly ControllerStateSubmissionPolicy _submissionPolicy = new();
    private HMGamepadState _state;
    private readonly Dictionary<uint, int> _touchSlots = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private int _hapticsSequence = -1;
    private int _hapticsStreaming;
    private int _hapticsNeedsStart;
    private int _asyncSubmitFailureReported;
    private int _disposed;
    private bool _stateDirty;
    private bool _stateFlushScheduled;
    private byte[] _audioResidual = Array.Empty<byte>();

    internal ControllerSession(byte deviceId,
                               byte clientControllerNumber,
                               HMController controller,
                               HMProfile profile,
                               Action<Protocol.Message> emit)
    {
        DeviceId = deviceId;
        ClientControllerNumber = clientControllerNumber;
        _controller = controller;
        _profile = profile;
        _emit = emit;
        _state = new HMGamepadState
        {
            Axes = HMGamepadStateHelpers.StandardAxes(profile),
            BatteryLevel = 10,
            BatteryFull = true,
        };

        _controller.OutputDecoded += OnOutputDecoded;
        if (DualSenseHapticsAudio.IsCompositeProfile(profile.Id))
        {
            _audioOutput = _controller.UsbAudio?.Output
                ?? throw new InvalidDataException("Composite DualSense did not expose its audio output");
            DualSenseHapticsAudio.ValidateRuntimeOutput(_audioOutput);
            _audioOutput.FramesReceived += OnAudioFrames;
            _audioOutput.StreamingChanged += OnAudioStreamingChanged;
        }
        // Emit a centered, untouched idle frame immediately: without it the
        // device reports an all-zero buffer until the first client input,
        // which raw consumers decode as a down touch contact at (0,0).
        _controller.SubmitState(in _state);
        _stateSubmitTimer = new System.Threading.Timer(
            _ => FlushScheduledState(),
            null,
            Timeout.InfiniteTimeSpan,
            Timeout.InfiniteTimeSpan);
    }

    internal byte DeviceId { get; }
    internal byte ClientControllerNumber { get; }
    internal bool HasAudio => _audioOutput is not null;

    internal void StartDefaultAudioEndpointGuard()
    {
        if (_audioOutput is null || _audioEndpointGuard is not null)
            return;
        _audioEndpointGuard = new DefaultAudioEndpointGuard(role =>
        {
            if (Volatile.Read(ref _disposed) != 0)
                return;
            _emit(new Protocol.Message(
                Protocol.MessageType.AudioPolicyViolation,
                0,
                new[] { DeviceId, ClientControllerNumber, (byte)role, (byte)0 }));
        });
    }

    internal void SubmitInput(ReadOnlySpan<byte> payload)
    {
        // id:u8, reserved:u8/u16, buttons:u32, lt/rt:u8, reserved:u16,
        // lsx/lsy/rsx/rsy:i16 = 20 bytes
        if (payload.Length != 20 || payload[0] != DeviceId)
            throw new InvalidDataException("Invalid input-state payload");

        var buttons = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
        var leftTrigger = payload[8] / 255.0f;
        var rightTrigger = payload[9] / 255.0f;
        var rawLeftX = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(12, 2));
        var rawLeftY = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(14, 2));
        var rawRightX = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(16, 2));
        var rawRightY = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(18, 2));
        var leftX = Axis(rawLeftX);
        var leftY = Axis(rawLeftY, invert: true);
        var rightX = Axis(rawRightX);
        var rightY = Axis(rawRightY, invert: true);
        var analogNeutral = payload[8] == 0 && payload[9] == 0 &&
                            rawLeftX == 0 && rawLeftY == 0 && rawRightX == 0 && rawRightY == 0;

        lock (_stateLock)
        {
            _state.Buttons = MapButtons(buttons);
            _state.Hat = MapHat(buttons);
            UpdateAxes(leftX, leftY, rightX, rightY, leftTrigger, rightTrigger);
            QueueStateSubmissionLocked(_submissionPolicy.ObserveInput(buttons, analogNeutral));
        }
    }

    internal void SubmitTouch(ReadOnlySpan<byte> payload)
    {
        // id:u8, event:u8, reserved:u16, pointer:u32, x/y/pressure:f32 = 20 bytes
        if (payload.Length != 20 || payload[0] != DeviceId)
            throw new InvalidDataException("Invalid touch payload");

        var eventType = payload[1];
        var pointerId = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
        var x = Math.Clamp(BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(8, 4))), 0, 1);
        var y = Math.Clamp(BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(12, 4))), 0, 1);

        lock (_stateLock)
        {
            var stateChanged = false;
            var stateBoundary = false;
            if (eventType == 7) // LI_TOUCH_EVENT_CANCEL_ALL
            {
                stateChanged = _touchSlots.Count != 0;
                stateBoundary = stateChanged;
                foreach (var slot in _touchSlots.Values.Distinct().ToArray())
                    SetTouch(slot, false, 0, 0, 0);
                _touchSlots.Clear();
            }
            else if (eventType is 1 or 3) // down/move claim or update a slot
            {
                if (!_touchSlots.TryGetValue(pointerId, out var slot))
                {
                    slot = !_touchSlots.ContainsValue(0) ? 0 : 1;
                    if (_touchSlots.ContainsValue(slot))
                        return;
                    _touchSlots[pointerId] = slot;
                    stateBoundary = true;
                }
                SetTouch(slot, true, pointerId, x, y);
                stateChanged = true;
            }
            else if (eventType is 0 or 6) // hover/hover-leave are not contacts
            {
                // A hover packet must never claim a finger slot. HID touchpads
                // use hover while the contact is up, and exposing it as active
                // makes Windows interpret ordinary pointer movement as a
                // precision-touchpad gesture (for example, menu scrolling).
                return;
            }
            else if (eventType is 2 or 4 or 6 && _touchSlots.Remove(pointerId, out var slot))
            {
                SetTouch(slot, false, pointerId, x, y);
                stateChanged = true;
                stateBoundary = true;
            }
            else if (eventType is not (2 or 4 or 5 or 6))
            {
                throw new InvalidDataException("Unsupported touch event type");
            }
            if (stateChanged)
                QueueStateSubmissionLocked(stateBoundary);
        }
    }

    internal void SubmitMotion(ReadOnlySpan<byte> payload)
    {
        // id:u8, type:u8, reserved:u16, x/y/z:f32 = 16 bytes
        if (payload.Length != 16 || payload[0] != DeviceId)
            throw new InvalidDataException("Invalid motion payload");
        var type = payload[1];
        var x = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(4, 4)));
        var y = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(8, 4)));
        var z = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(12, 4)));

        lock (_stateLock)
        {
            if (type == 1)
            {
                _state.AccelGX = x / 9.80665f;
                _state.AccelGY = y / 9.80665f;
                _state.AccelGZ = z / 9.80665f;
                // The alwaysArmed extendedReport encodes sensors from the raw
                // Sony firmware fields, not the calibrated ones. Scales follow
                // SDL's hidapi_ps5: 8192 LSB per g, HID gyro units are 1/64 of
                // 1024 LSB per deg/s (= 16 LSB per deg/s), pitch/yaw/roll map
                // to SDL gyro X/Y/Z one-to-one.
                _state.AccelX = RawAccel(_state.AccelGX);
                _state.AccelY = RawAccel(_state.AccelGY);
                _state.AccelZ = RawAccel(_state.AccelGZ);
            }
            else if (type == 2)
            {
                _state.GyroDpsX = x;
                _state.GyroDpsY = y;
                _state.GyroDpsZ = z;
                _state.GyroPitch = RawGyro(x);
                _state.GyroYaw = RawGyro(y);
                _state.GyroRoll = RawGyro(z);
            }
            else
            {
                throw new InvalidDataException("Unsupported motion type");
            }
            _state.SensorTimestamp = unchecked((uint)ElapsedMicroseconds());
            QueueStateSubmissionLocked();
        }
    }

    internal void SubmitBattery(ReadOnlySpan<byte> payload)
    {
        // id:u8, state:u8, percentage:u8, reserved:u8
        if (payload.Length != 4 || payload[0] != DeviceId)
            throw new InvalidDataException("Invalid battery payload");
        lock (_stateLock)
        {
            _state.BatteryLevel = payload[2] == 0xFF
                ? (byte)10
                : (byte)Math.Clamp((payload[2] + 5) / 10, 0, 10);
            _state.BatteryCharging = payload[1] == 3;
            _state.BatteryFull = payload[1] == 5;
            QueueStateSubmissionLocked();
        }
    }

    private void QueueStateSubmissionLocked(bool immediate = false)
    {
        if (Volatile.Read(ref _disposed) != 0)
            return;

        _stateDirty = true;
        if (immediate)
        {
            if (_stateFlushScheduled)
            {
                _stateFlushScheduled = false;
                _stateSubmitTimer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
            }
            SubmitPendingStateLocked();
        }
        else if (!_stateFlushScheduled)
        {
            _stateFlushScheduled = true;
            _stateSubmitTimer.Change(StateCoalesceWindow, Timeout.InfiniteTimeSpan);
        }
    }

    private void FlushScheduledState()
    {
        try
        {
            lock (_stateLock)
            {
                if (Volatile.Read(ref _disposed) != 0 || !_stateFlushScheduled)
                    return;
                _stateFlushScheduled = false;
                SubmitPendingStateLocked();
            }
        }
        catch (Exception ex)
        {
            if (Interlocked.Exchange(ref _asyncSubmitFailureReported, 1) == 0)
            {
                _emit(new Protocol.Message(
                    Protocol.MessageType.Error,
                    0,
                    Protocol.ErrorPayload(-1, $"Unable to submit a coalesced DualSense state: {ex.Message}")));
            }
        }
    }

    private void SubmitPendingStateLocked()
    {
        if (!_stateDirty)
            return;
        _controller.SubmitState(in _state);
        _stateDirty = false;
    }

    private void OnOutputDecoded(object? sender, HMOutputDecodedEventArgs output)
    {
        if (!output.CrcValid)
            return;

        lock (_outputLock)
        {
            if (Volatile.Read(ref _disposed) != 0)
                return;

            if (TryByte(output.Fields, "leftMotor", out var left) &&
                TryByte(output.Fields, "rightMotor", out var right))
            {
                var payload = new byte[6];
                payload[0] = DeviceId;
                payload[1] = ClientControllerNumber;
                BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(2, 2), (ushort)(left * 257));
                BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(4, 2), (ushort)(right * 257));
                _emit(new Protocol.Message(Protocol.MessageType.Rumble, 0, payload));
            }

            if (_adaptiveTriggers.TryUpdate(output.Fields, DeviceId, ClientControllerNumber, out var adaptiveTriggers))
                _emit(adaptiveTriggers);

            if (output.Fields.TryGetValue("lightbar", out var value) && value is byte[] rgb && rgb.Length >= 3)
            {
                _emit(new Protocol.Message(
                    Protocol.MessageType.Led,
                    0,
                    new[] { DeviceId, ClientControllerNumber, rgb[0], rgb[1], rgb[2] }));
            }
        }
    }

    private void OnAudioStreamingChanged(object? sender, bool streaming)
    {
        if (streaming)
        {
            // Arm the start marker before publishing the streaming flag, or a
            // frame racing this callback could be emitted mid-stream without
            // the StreamStart marker the client resets on.
            Interlocked.Exchange(ref _hapticsNeedsStart, 1);
            Interlocked.Exchange(ref _hapticsStreaming, 1);
        }
        else
        {
            Interlocked.Exchange(ref _hapticsStreaming, 0);
            // A stale sub-frame tail from the old stream must not splice into
            // the first frame of the next stream.
            _audioResidual = Array.Empty<byte>();
            EmitHaptics(ReadOnlySpan<byte>.Empty, 0, Protocol.HapticsFlags.StreamEnd);
        }
    }

    private void OnAudioFrames(object? sender, ReadOnlyMemory<byte> pcm)
    {
        // Only the USB audio output thread raises this callback, so the
        // residual carry is not guarded by a lock.
        byte[] combined;
        if (_audioResidual.Length == 0)
        {
            combined = pcm.ToArray();
        }
        else
        {
            combined = new byte[_audioResidual.Length + pcm.Length];
            _audioResidual.AsSpan().CopyTo(combined);
            pcm.Span.CopyTo(combined.AsSpan(_audioResidual.Length));
        }
        var sourceFrameBytes = DualSenseHapticsAudio.InputFrameBytes;
        var usableBytes = combined.Length - combined.Length % sourceFrameBytes;
        _audioResidual = usableBytes == combined.Length ? Array.Empty<byte>() : combined[usableBytes..];

        var source = combined.AsSpan(0, usableBytes);
        var frameCount = source.Length / sourceFrameBytes;
        var offsetFrames = 0;
        while (offsetFrames < frameCount)
        {
            var frames = Math.Min(HapticsFramesPerPacket, frameCount - offsetFrames);
            var haptics = DualSenseHapticsAudio.Extract(
                source.Slice(offsetFrames * sourceFrameBytes, frames * sourceFrameBytes));
            var flags = Volatile.Read(ref _hapticsStreaming) != 0 &&
                        Interlocked.Exchange(ref _hapticsNeedsStart, 0) != 0
                ? Protocol.HapticsFlags.StreamStart
                : Protocol.HapticsFlags.None;
            EmitHaptics(haptics, (ushort)frames, flags);
            offsetFrames += frames;
        }
    }

    private void EmitHaptics(ReadOnlySpan<byte> pcm, ushort frameCount, Protocol.HapticsFlags flags)
    {
        // id:u8, controller:u8, flags:u8, channels:u8, frames:u16,
        // bits:u8, reserved:u8, seq:u32, timestamp:u64, rate:u32, PCM
        var payload = new byte[24 + pcm.Length];
        payload[0] = DeviceId;
        payload[1] = ClientControllerNumber;
        payload[2] = (byte)flags;
        payload[3] = DualSenseHapticsAudio.OutputChannels;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(4, 2), frameCount);
        payload[6] = DualSenseHapticsAudio.BitsPerSample;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4),
            unchecked((uint)Interlocked.Increment(ref _hapticsSequence)));
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(12, 8),
            (ulong)ElapsedMicroseconds());
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), DualSenseHapticsAudio.SampleRateHz);
        pcm.CopyTo(payload.AsSpan(24));
        _emit(new Protocol.Message(Protocol.MessageType.HapticsPcm, 0, payload));
    }

    private long ElapsedMicroseconds()
    {
        var ticks = _clock.ElapsedTicks;
        return ticks / Stopwatch.Frequency * 1_000_000 +
               ticks % Stopwatch.Frequency * 1_000_000 / Stopwatch.Frequency;
    }

    private void UpdateAxes(float lx, float ly, float rx, float ry, float lt, float rt)
    {
        var values = HMGamepadStateHelpers.StandardAxes(_profile, lx, ly, rx, ry, lt, rt);
        foreach (var (axis, value) in values)
            _state.Axes![axis] = value;
    }

    private void SetTouch(int slot, bool active, uint pointerId, float x, float y)
    {
        var px = (ushort)Math.Clamp((int)Math.Round(x * 1919), 0, 1919);
        var py = (ushort)Math.Clamp((int)Math.Round(y * 1079), 0, 1079);
        if (slot == 0)
        {
            _state.TouchpadFinger0Active = active;
            _state.TouchpadFinger0Id = (byte)(pointerId & 0x7F);
            _state.TouchpadFinger0X = px;
            _state.TouchpadFinger0Y = py;
        }
        else
        {
            _state.TouchpadFinger1Active = active;
            _state.TouchpadFinger1Id = (byte)(pointerId & 0x7F);
            _state.TouchpadFinger1X = px;
            _state.TouchpadFinger1Y = py;
        }
    }

    private static short RawAccel(float g) =>
        (short)Math.Clamp((int)Math.Round(g * 8192f), short.MinValue, short.MaxValue);

    private static short RawGyro(float degreesPerSecond) =>
        (short)Math.Clamp((int)Math.Round(degreesPerSecond * 16f), short.MinValue, short.MaxValue);

    private static HMButton MapButtons(uint flags)
    {
        var result = HMButton.None;
        if ((flags & A) != 0) result |= HMButton.A;
        if ((flags & B) != 0) result |= HMButton.B;
        if ((flags & X) != 0) result |= HMButton.X;
        if ((flags & Y) != 0) result |= HMButton.Y;
        if ((flags & LeftBumper) != 0) result |= HMButton.LeftBumper;
        if ((flags & RightBumper) != 0) result |= HMButton.RightBumper;
        if ((flags & Back) != 0) result |= HMButton.Back;
        if ((flags & Start) != 0) result |= HMButton.Start;
        if ((flags & LeftStick) != 0) result |= HMButton.LeftStick;
        if ((flags & RightStick) != 0) result |= HMButton.RightStick;
        if ((flags & Guide) != 0) result |= HMButton.Guide;
        if ((flags & Touchpad) != 0) result |= HMButton.Touchpad;
        if ((flags & Misc) != 0) result |= HMButton.Misc1;
        return result;
    }

    private static HMHat MapHat(uint flags)
    {
        var up = (flags & DpadUp) != 0;
        var down = (flags & DpadDown) != 0;
        var left = (flags & DpadLeft) != 0;
        var right = (flags & DpadRight) != 0;
        if (up && right) return HMHat.NorthEast;
        if (down && right) return HMHat.SouthEast;
        if (down && left) return HMHat.SouthWest;
        if (up && left) return HMHat.NorthWest;
        if (up) return HMHat.North;
        if (right) return HMHat.East;
        if (down) return HMHat.South;
        if (left) return HMHat.West;
        return HMHat.None;
    }

    private static float Axis(short value, bool invert = false)
    {
        var normalized = (value - (float)short.MinValue) / ushort.MaxValue;
        return invert ? 1.0f - normalized : normalized;
    }

    private static bool TryByte(IReadOnlyDictionary<string, object> fields, string name, out byte value)
    {
        if (fields.TryGetValue(name, out var item) && item is byte b)
        {
            value = b;
            return true;
        }
        value = 0;
        return false;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        lock (_stateLock)
        {
            // Wait for an in-flight timer callback before disposing the HID
            // controller, and prevent a stale state from crossing sessions.
            _stateSubmitTimer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
            _stateSubmitTimer.Dispose();
            _stateDirty = false;
            _stateFlushScheduled = false;
        }
        _audioEndpointGuard?.Dispose();
        _audioEndpointGuard = null;
        _controller.OutputDecoded -= OnOutputDecoded;
        lock (_outputLock)
        {
            if (_adaptiveTriggers.TryReset(DeviceId, ClientControllerNumber, out var adaptiveTriggers))
                _emit(adaptiveTriggers);
        }
        if (_audioOutput is not null)
        {
            _audioOutput.FramesReceived -= OnAudioFrames;
            _audioOutput.StreamingChanged -= OnAudioStreamingChanged;
        }
        _controller.Dispose();
    }
}
