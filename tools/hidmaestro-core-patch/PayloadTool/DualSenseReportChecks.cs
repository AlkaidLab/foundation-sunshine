using System.Buffers.Binary;
using System.Reflection;
using System.Text.Json;

internal static class DualSenseReportChecks
{
    internal static void Run(Assembly core, string profilePath)
    {
        var codec = core.GetType("HIDMaestro.Internal.VendorBlobCodec", true)!;
        var stateType = core.GetType("HIDMaestro.HMGamepadState", true)!;
        var specType = core.GetType("HIDMaestro.Internal.ExtendedReportSpec", true)!;
        var encoderType = codec.GetNestedType("EncoderState")!;
        var encode = codec.GetMethod("EncodeInput")!;
        var clock = core.GetType("Foundation.Input.DualSenseClock", true)!
            .GetProperty("Now", BindingFlags.Static | BindingFlags.NonPublic)!;
        uint Now() => (uint)clock.GetValue(null)!;
        using var document = JsonDocument.Parse(File.ReadAllText(profilePath));
        var spec = JsonSerializer.Deserialize(document.RootElement.GetProperty("extendedReport").GetRawText(), specType)!;
        var encoder = Activator.CreateInstance(encoderType)!;
        var state = Activator.CreateInstance(stateType)!;
        void Set(string name, object value) => stateType.GetField(name)!.SetValue(state, value);
        byte[] Encode()
        {
            var bytes = new byte[64];
            encode.Invoke(null, new object[] { spec, state, .5f, .5f, .5f, .5f, 0f, 0f, bytes, encoder });
            return bytes;
        }
        static uint U32(byte[] bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset));
        static void Check(bool ok, string message)
        {
            if (!ok) throw new InvalidOperationException("DualSense report: " + message);
        }

        // Construction time must not become a separate timestamp epoch.
        Thread.Sleep(25);
        var sensorTime = Now();
        Set("SensorTimestamp", sensorTime);
        Set("GyroYaw", (short)480);
        Set("AccelY", (short)8192);
        var counters = (Dictionary<string, uint>)encoderType.GetProperty("RollingCounters32")!.GetValue(encoder)!;
        counters["packetSequence"] = uint.MaxValue - 1;
        uint previousDevice = 0;
        for (int i = 0; i < 260; i++)
        {
            // Button-only reports retain the previous motion sample time.
            Set("Buttons", Enum.ToObject(stateType.GetField("Buttons")!.FieldType, i % 2 == 0 ? 1 : 0));
            var before = Now();
            var bytes = Encode();
            var after = Now();
            var device = U32(bytes, 49);
            Check(unchecked(device - before) <= unchecked(after - before), "device timestamp has a different epoch");
            Check(i == 0 || unchecked(device - previousDevice) < 0x80000000u, "device clock moved backwards");
            previousDevice = device;
            Check(U32(bytes, 28) == sensorTime, "button report changed the sensor timestamp");
            Check(bytes[7] == unchecked((byte)i), "8-bit sequence or wrap");
            Check(U32(bytes, 12) == unchecked(uint.MaxValue - 1 + (uint)i), "32-bit sequence or wrap");
            Check(bytes[54] == 8, "USB connection state");
            Check((bytes[8] & 0x20) == (i % 2 == 0 ? 0x20 : 0), "button press/release");
            Check(BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(18)) == 480 &&
                BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(24)) == 8192, "motion values changed");
        }
        Thread.Sleep(25);
        Check(unchecked(U32(Encode(), 49) - previousDevice) >= 60_000, "device clock stopped without motion");
        // Zero is a valid sensor timestamp at uint32 rollover, not a request
        // to substitute another clock or manufacture a new motion sample.
        Set("SensorTimestamp", 0u);
        Check(U32(Encode(), 28) == 0, "sensor rollover was replaced");
        Console.WriteLine($"PASS: {Path.GetFileName(profilePath)} clocks, counters, USB state, button edges and motion bytes");
    }
}
