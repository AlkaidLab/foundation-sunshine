namespace Sunshine.Ds5Sidecar;

/// <summary>
/// Reads complete frames without making a separate pipe read and allocation
/// for every header and payload. Buffered frames are dispatched immediately.
/// </summary>
internal sealed class ProtocolFrameReader
{
    internal readonly record struct Frame(Protocol.Header Header, ReadOnlyMemory<byte> Payload);

    private byte[] _buffer;
    private int _start;
    private int _end;

    internal ProtocolFrameReader(int initialCapacity = 4096)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(initialCapacity, Protocol.HeaderSize);
        _buffer = new byte[initialCapacity];
    }

    // Payload borrows the reader's buffer and is valid only until the next
    // ReadAsync call. A consumer retaining it (microphone PCM) must copy it.
    internal async ValueTask<Frame> ReadAsync(Stream stream, CancellationToken cancellationToken)
    {
        // Keep the read in one async state machine. Nesting an async
        // EnsureAvailable call allocated an additional box per pipe wakeup.
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var length = Protocol.HeaderSize;
            if (_end - _start >= Protocol.HeaderSize)
            {
                var header = Protocol.DecodeHeader(_buffer.AsSpan(_start, Protocol.HeaderSize));
                length = checked(Protocol.HeaderSize + (int)header.PayloadLength);
                if (_end - _start >= length)
                {
                    var payload = _buffer.AsMemory(_start + Protocol.HeaderSize, (int)header.PayloadLength);
                    _start += length;
                    return new Frame(header, payload);
                }
            }

            if (_start == _end)
                _start = _end = 0;
            else if (_buffer.Length - _start < length)
            {
                _buffer.AsSpan(_start, _end - _start).CopyTo(_buffer);
                _end -= _start;
                _start = 0;
            }

            if (_buffer.Length < length)
                Array.Resize(ref _buffer, length);

            var read = await stream.ReadAsync(_buffer.AsMemory(_end), cancellationToken);
            if (read == 0)
                throw new EndOfStreamException();
            _end += read;
        }
    }
}
