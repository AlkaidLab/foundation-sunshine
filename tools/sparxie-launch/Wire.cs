using System.Buffers.Binary;
using System.Text;

namespace Sparxie.Launch;

// Clean-room encoding of the version-1 Broker contract: only the observed protobuf field numbers and the documented gRPC framing
// (source index: sparxie-broker-contract-031efe2). No Sparxie code is used.

internal static class Grpc
{
    public static byte[] Frame(byte[] payload)
    {
        var frame = new byte[payload.Length + 5];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(1, 4), (uint)payload.Length);
        payload.CopyTo(frame, 5);
        return frame;
    }

    /// <summary>Reads one length-prefixed message; null at a clean end of stream.</summary>
    public static async Task<byte[]?> ReadFrameAsync(Stream stream, CancellationToken cancellation)
    {
        var header = new byte[5];
        if (!await ReadExactAsync(stream, header, cancellation).ConfigureAwait(false)) return null;
        if (header[0] != 0) throw new InvalidDataException("the Broker sent a compressed gRPC message");
        var length = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(1, 4));
        if (length > 1024 * 1024) throw new InvalidDataException("the Broker sent an oversized gRPC message");
        var body = new byte[length];
        if (!await ReadExactAsync(stream, body, cancellation).ConfigureAwait(false)) throw new EndOfStreamException("the Broker message was cut short");
        return body;
    }

    private static async Task<bool> ReadExactAsync(Stream stream, byte[] buffer, CancellationToken cancellation)
    {
        var at = 0;
        while (at < buffer.Length)
        {
            var n = await stream.ReadAsync(buffer.AsMemory(at), cancellation).ConfigureAwait(false);
            if (n == 0) { if (at == 0) return false; throw new EndOfStreamException("the Broker message was cut short"); }
            at += n;
        }
        return true;
    }
}

internal readonly record struct ProtoField(int Number, int Wire, ulong Varint, byte[] Bytes);

internal static class Proto
{
    public readonly record struct Value(byte[] Bytes, bool LengthDelimited);

    public static Value Int(int value) => new(Var((ulong)(uint)value), false);
    public static Value Bool(bool value) => new(Var(value ? 1UL : 0UL), false);
    public static Value String(string value) => new(Encoding.UTF8.GetBytes(value), true);
    public static Value Bytes(byte[] value) => new(value, true);

    public static byte[] Message(params (int Field, Value Value)[] fields)
    {
        using var output = new MemoryStream();
        foreach (var (field, value) in fields) Write(output, field, value);
        return output.ToArray();
    }

    public static byte[] Append(byte[] message, int field, byte[] nested)
    {
        using var output = new MemoryStream();
        output.Write(message);
        Write(output, field, new Value(nested, true));
        return output.ToArray();
    }

    private static void Write(Stream output, int field, Value value)
    {
        WriteVar(output, (ulong)((field << 3) | (value.LengthDelimited ? 2 : 0)));
        if (value.LengthDelimited) WriteVar(output, (ulong)value.Bytes.Length);
        output.Write(value.Bytes);
    }

    private static byte[] Var(ulong value) { using var output = new MemoryStream(); WriteVar(output, value); return output.ToArray(); }

    private static void WriteVar(Stream output, ulong value)
    {
        while (value >= 128) { output.WriteByte((byte)(value | 128)); value >>= 7; }
        output.WriteByte((byte)value);
    }

    /// <summary>Splits a message into its fields; unknown wire types of fixed size are skipped, a malformed one throws.</summary>
    public static List<ProtoField> Parse(byte[] data)
    {
        var list = new List<ProtoField>();
        var at = 0;
        while (at < data.Length)
        {
            var key = ReadVar(data, ref at);
            var number = (int)(key >> 3);
            var wire = (int)(key & 7);
            switch (wire)
            {
                case 0: list.Add(new ProtoField(number, 0, ReadVar(data, ref at), Array.Empty<byte>())); break;
                case 1: Skip(data, ref at, 8); break;
                case 5: Skip(data, ref at, 4); break;
                case 2:
                    var length = ReadVar(data, ref at);
                    if (length > (ulong)(data.Length - at)) throw new InvalidDataException("protobuf field is longer than the message");
                    list.Add(new ProtoField(number, 2, 0, data[at..(at + (int)length)]));
                    at += (int)length;
                    break;
                default: throw new InvalidDataException($"unsupported protobuf wire type {wire}");
            }
        }
        return list;
    }

    private static void Skip(byte[] data, ref int at, int count)
    {
        if (count > data.Length - at) throw new InvalidDataException("protobuf field is longer than the message");
        at += count;
    }

    public static string String(List<ProtoField> fields, int number)
    {
        foreach (var f in fields) if (f.Number == number && f.Wire == 2) return Encoding.UTF8.GetString(f.Bytes);
        return "";
    }

    public static int Int(List<ProtoField> fields, int number)
    {
        foreach (var f in fields) if (f.Number == number && f.Wire == 0) return (int)(uint)f.Varint;
        return 0;
    }

    public static bool Bool(List<ProtoField> fields, int number) => Int(fields, number) != 0;

    private static ulong ReadVar(byte[] data, ref int at)
    {
        ulong value = 0;
        var shift = 0;
        while (at < data.Length)
        {
            var b = data[at++];
            value |= (ulong)(b & 127) << shift;
            if ((b & 128) == 0) return value;
            shift += 7;
            if (shift > 63) throw new InvalidDataException("invalid protobuf varint");
        }
        throw new EndOfStreamException("protobuf varint was cut short");
    }
}
