using System.Buffers;

namespace Sunshine.Ds5Sidecar;

internal sealed class MicrophonePcmQueue : IDisposable
{
    internal enum EnqueueResult
    {
        Accepted,
        GenerationMismatch,
    }

    internal sealed class Block : IDisposable
    {
        private byte[]? _buffer;

        internal Block(byte[] buffer, int length, uint sequence, ulong captureTimeUs,
                       Protocol.MicPcmFlags flags)
        {
            _buffer = buffer;
            Length = length;
            Sequence = sequence;
            CaptureTimeUs = captureTimeUs;
            Flags = flags;
        }

        internal int Length { get; }
        internal int FrameCount => Length / 2;
        internal uint Sequence { get; }
        internal ulong CaptureTimeUs { get; }
        internal Protocol.MicPcmFlags Flags { get; private set; }
        internal ReadOnlySpan<byte> Pcm => (_buffer ?? throw new ObjectDisposedException(nameof(Block))).AsSpan(0, Length);

        internal void MarkDiscontinuity() => Flags |= Protocol.MicPcmFlags.Discontinuity;

        public void Dispose()
        {
            var buffer = Interlocked.Exchange(ref _buffer, null);
            if (buffer is not null)
                ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private readonly Queue<Block> _blocks = new();
    private readonly object _sync = new();
    private readonly int _targetFrames;
    private readonly int _maximumFrames;
    private uint _generation;
    private uint _nextSequence;
    private bool _hasSequence;
    private int _bufferedFrames;
    private uint _droppedFrames;
    private uint _sequenceGaps;
    private uint _generationMismatches;

    internal MicrophonePcmQueue(int targetFrames = 960, int maximumFrames = 2_880)
    {
        if (targetFrames <= 0 || maximumFrames < targetFrames)
            throw new ArgumentOutOfRangeException(nameof(maximumFrames));
        _targetFrames = targetFrames;
        _maximumFrames = maximumFrames;
    }

    internal uint Generation { get { lock (_sync) return _generation; } }
    internal int BufferedFrames { get { lock (_sync) return _bufferedFrames; } }
    internal int BufferedBytes { get { lock (_sync) return checked(_bufferedFrames * 2); } }
    internal uint DroppedFrames { get { lock (_sync) return _droppedFrames; } }
    internal uint SequenceGaps { get { lock (_sync) return _sequenceGaps; } }
    internal uint GenerationMismatches { get { lock (_sync) return _generationMismatches; } }

    internal void Reset(uint generation)
    {
        lock (_sync)
        {
            ClearBlocks(countAsDropped: false);
            _generation = generation;
            _hasSequence = false;
        }
    }

    internal EnqueueResult Enqueue(Protocol.MicPcmPacket packet)
    {
        lock (_sync)
        {
            if (packet.Generation != _generation)
            {
                _generationMismatches = SaturatingIncrement(_generationMismatches);
                _droppedFrames = SaturatingAdd(_droppedFrames, packet.FrameCount);
                return EnqueueResult.GenerationMismatch;
            }

            var flags = packet.Flags;
            if (_hasSequence && packet.Sequence != _nextSequence)
            {
                _sequenceGaps = SaturatingIncrement(_sequenceGaps);
                ClearBlocks(countAsDropped: true);
                flags |= Protocol.MicPcmFlags.Discontinuity;
            }

            _hasSequence = true;
            _nextSequence = unchecked(packet.Sequence + 1);
            var buffer = ArrayPool<byte>.Shared.Rent(packet.Pcm.Length);
            packet.Pcm.Span.CopyTo(buffer);
            _blocks.Enqueue(new Block(buffer, packet.Pcm.Length, packet.Sequence, packet.CaptureTimeUs, flags));
            _bufferedFrames = checked(_bufferedFrames + packet.FrameCount);

            if (_bufferedFrames > _maximumFrames)
            {
                while (_bufferedFrames > _targetFrames && _blocks.TryDequeue(out var stale))
                {
                    _bufferedFrames -= stale.FrameCount;
                    _droppedFrames = SaturatingAdd(_droppedFrames, (uint)stale.FrameCount);
                    stale.Dispose();
                }
                if (_blocks.TryPeek(out var next))
                    next.MarkDiscontinuity();
            }

            return EnqueueResult.Accepted;
        }
    }

    internal bool TryDequeue(out Block? block)
    {
        lock (_sync)
        {
            if (!_blocks.TryDequeue(out block))
                return false;
            _bufferedFrames -= block.FrameCount;
            return true;
        }
    }

    internal void Flush()
    {
        lock (_sync)
        {
            ClearBlocks(countAsDropped: false);
            _hasSequence = false;
        }
    }

    private void ClearBlocks(bool countAsDropped)
    {
        while (_blocks.TryDequeue(out var block))
        {
            if (countAsDropped)
                _droppedFrames = SaturatingAdd(_droppedFrames, (uint)block.FrameCount);
            block.Dispose();
        }
        _bufferedFrames = 0;
    }

    private static uint SaturatingIncrement(uint value) =>
        value == uint.MaxValue ? value : value + 1;

    private static uint SaturatingAdd(uint value, uint addition) =>
        uint.MaxValue - value < addition ? uint.MaxValue : value + addition;

    public void Dispose()
    {
        lock (_sync)
            ClearBlocks(countAsDropped: false);
    }
}
