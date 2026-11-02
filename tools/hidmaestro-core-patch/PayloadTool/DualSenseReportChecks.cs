using System.Buffers.Binary;
using System.Reflection;
using System.Text.Json;

internal static class DualSenseReportChecks
{
    /// <summary>Encodes one USB profile without creating a device and checks clocks, counters and retained input data.</summary>
    internal static void Run(Assembly core, string profilePath, Action<bool, string> require)
    {
        var codec = core.GetType("HIDMaestro.Internal.VendorBlobCodec", true)!;
        var stateType = core.GetType("HIDMaestro.HMGamepadState", true)!;
        var specType = core.GetType("HIDMaestro.Internal.ExtendedReportSpec", true)!;
        var encode = codec.GetMethod("EncodeInput")!;
        var clock = core.GetType("Foundation.Input.DualSenseClock", true)!
            .GetProperty("Now", BindingFlags.Static | BindingFlags.NonPublic)!;
        uint Now() => (uint)clock.GetValue(null)!;
        using var document = JsonDocument.Parse(File.ReadAllText(profilePath));
        var spec = JsonSerializer.Deserialize(document.RootElement.GetProperty("extendedReport").GetRawText(), specType)!;
        var encoder = Activator.CreateInstance(codec.GetNestedType("EncoderState")!)!;
        var state = Activator.CreateInstance(stateType)!;
        void Set(string name, object value) => stateType.GetField(name)!.SetValue(state, value);
        byte[] Encode(bool pressed)
        {
            Set("Buttons", Enum.ToObject(stateType.GetField("Buttons")!.FieldType, pressed ? 1 : 0));
            var bytes = new byte[64];
            var before = Now();
            encode.Invoke(null, new object[] { spec, state, .5f, .5f, .5f, .5f, 0f, 0f, bytes, encoder });
            require(unchecked(U32(bytes, 49) - before) <= unchecked(Now() - before), "device clock epoch");
            return bytes;
        }
        static uint U32(byte[] bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset));

        var sensorTime = Now();
        Set("SensorTimestamp", sensorTime);
        Set("GyroYaw", (short)480);
        Set("AccelY", (short)8192);
        var counters = (Dictionary<string, uint>)encoder.GetType().GetProperty("RollingCounters32")!.GetValue(encoder)!;
        counters["packetSequence"] = uint.MaxValue;
        var down = Encode(true);
        Thread.Sleep(25);
        var up = Encode(false);
        var elapsed = unchecked(U32(up, 49) - U32(down, 49));
        require(elapsed >= 60_000 && elapsed < 0x80000000u, "device clock must advance without motion");
        require(U32(down, 12) == uint.MaxValue && U32(up, 12) == 0, "32-bit sequence rollover");
        require(down[54] == 8 && up[54] == 8, "USB connection state");
        require((down[8] & 0x20) != 0 && (up[8] & 0x20) == 0, "button press/release");
        require(BinaryPrimitives.ReadInt16LittleEndian(down.AsSpan(18)) == 480 &&
            BinaryPrimitives.ReadInt16LittleEndian(down.AsSpan(24)) == 8192 && U32(down, 28) == sensorTime &&
            down.AsSpan(16, 16).SequenceEqual(up.AsSpan(16, 16)), "motion values and sample time must be retained");
        Set("SensorTimestamp", 0u);
        require(U32(Encode(false), 28) == 0, "zero at sensor timestamp rollover must be retained");
        Console.WriteLine($"PASS: {Path.GetFileName(profilePath)} clocks, counters, USB state, button edges and motion bytes");
    }
}
