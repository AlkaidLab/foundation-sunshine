using System.Text.Json;
using System.Text.Json.Nodes;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal static class DualSenseHapticsAudio
{
    internal const string CompositeProfileId = "dualsense-composite";
    internal const string GenshinCompatibilityProfileId = "dualsense-composite-genshin";
    internal const string CompositeProductString = "DualSense Wireless Controller";
    internal const string GenshinCompatibilityProductString = "Wireless Controller";
    internal const int InputChannels = 4;
    internal const int OutputChannels = 2;
    internal const int BitsPerSample = 16;
    internal const int BytesPerSample = BitsPerSample / 8;
    internal const int SampleRateHz = 48000;
    internal const int ChannelConfig = 0x0033;
    internal const int InputFrameBytes = InputChannels * BytesPerSample;
    internal const int OutputFrameBytes = OutputChannels * BytesPerSample;

    private static readonly string[] ExpectedRoles =
    [
        "speakerLeft",
        "speakerRight",
        "hapticLeft",
        "hapticRight",
    ];

    internal static void ValidateCompositeProfile(ReadOnlyMemory<byte> json)
    {
        ValidateCompositeProfile(json, CompositeProfileId, CompositeProductString);
    }

    internal static byte[] CreateRuntimeCompositeProfile(ReadOnlyMemory<byte> compositeJson)
    {
        var root = JsonNode.Parse(compositeJson.Span)?.AsObject()
                   ?? throw new InvalidDataException("Composite profile is not a JSON object");
        var controls = root["usbConfiguration"]?["audioControls"]?.AsArray()
                       ?? throw new InvalidDataException("Composite profile has no USB audio controls");
        var speaker = controls.Select(control => control?.AsObject())
            .FirstOrDefault(control => control?["function"]?.GetValue<string>() == "speaker")
            ?? throw new InvalidDataException("Composite profile has no speaker audio control");
        speaker["volumeCurRaw"] = speaker["volumeMaxRaw"]?.GetValue<int>()
                                  ?? throw new InvalidDataException("Composite profile speaker has no maximum volume");

        var runtimeProfile = JsonSerializer.SerializeToUtf8Bytes(root);
        ValidateCompositeProfile(runtimeProfile);
        return runtimeProfile;
    }

    internal static byte[] CreateGenshinCompatibilityProfile(ReadOnlyMemory<byte> compositeJson)
    {
        var root = JsonNode.Parse(compositeJson.Span)?.AsObject()
                   ?? throw new InvalidDataException("Composite profile is not a JSON object");
        root["id"] = GenshinCompatibilityProfileId;
        root["name"] = "DualSense (PS5) — Genshin compatibility";
        root["productString"] = GenshinCompatibilityProductString;
        var derived = JsonSerializer.SerializeToUtf8Bytes(root);
        ValidateCompositeProfile(
            derived, GenshinCompatibilityProfileId, GenshinCompatibilityProductString);
        return derived;
    }

    internal static bool IsCompositeProfile(string profileId)
    {
        return profileId is CompositeProfileId or GenshinCompatibilityProfileId;
    }

    private static void ValidateCompositeProfile(
        ReadOnlyMemory<byte> json, string expectedId, string expectedProductString)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        RequireString(root, "id", expectedId);
        RequireString(root, "productString", expectedProductString);
        RequireString(root, "backend", "usbip");

        if (!root.TryGetProperty("usbConfiguration", out var configuration) ||
            !configuration.TryGetProperty("interfaces", out var interfaces) ||
            interfaces.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidDataException("Composite profile has no USB interface list");
        }

        JsonElement? outputStream = null;
        foreach (var usbInterface in interfaces.EnumerateArray())
        {
            if (!usbInterface.TryGetProperty("function", out var function) ||
                function.GetString() != "audioStreamingOut" ||
                !usbInterface.TryGetProperty("altSettings", out var altSettings) ||
                altSettings.ValueKind != JsonValueKind.Array)
            {
                continue;
            }

            foreach (var altSetting in altSettings.EnumerateArray())
            {
                if (!altSetting.TryGetProperty("audioStream", out var candidate))
                    continue;
                if (outputStream is not null)
                    throw new InvalidDataException("Composite profile has multiple output audio streams");
                outputStream = candidate;
            }
        }

        if (outputStream is null)
            throw new InvalidDataException("Composite profile has no output audio stream");
        ValidateFormat(outputStream.Value);
        ValidateSpeakerControl(root);
    }

    internal static void ValidateRuntimeOutput(HMAudioOutput output)
    {
        if (output.Channels != InputChannels ||
            output.BitsPerSample != BitsPerSample ||
            output.SampleRateHz != SampleRateHz ||
            !output.ChannelRoles.SequenceEqual(ExpectedRoles, StringComparer.Ordinal))
        {
            throw new InvalidDataException(
                $"Unexpected DualSense audio layout: {output.Channels}ch/{output.BitsPerSample}-bit/" +
                $"{output.SampleRateHz}Hz [{string.Join(", ", output.ChannelRoles)}]");
        }
    }

    internal static byte[] Extract(ReadOnlySpan<byte> interleavedFrames)
    {
        if (interleavedFrames.Length % InputFrameBytes != 0)
            throw new InvalidDataException("DualSense audio data does not contain complete four-channel frames");

        var frameCount = interleavedFrames.Length / InputFrameBytes;
        var haptics = new byte[frameCount * OutputFrameBytes];
        for (var frame = 0; frame < frameCount; frame++)
        {
            var sourceOffset = frame * InputFrameBytes + 2 * BytesPerSample;
            interleavedFrames.Slice(sourceOffset, OutputFrameBytes)
                .CopyTo(haptics.AsSpan(frame * OutputFrameBytes, OutputFrameBytes));
        }
        return haptics;
    }

    private static void ValidateFormat(JsonElement stream)
    {
        RequireInt(stream, "channels", InputChannels);
        RequireInt(stream, "bitsPerSample", BitsPerSample);
        RequireInt(stream, "sampleRateHz", SampleRateHz);
        RequireInt(stream, "channelConfig", ChannelConfig);

        if (!stream.TryGetProperty("channelRoles", out var roles) ||
            roles.ValueKind != JsonValueKind.Array ||
            roles.GetArrayLength() != ExpectedRoles.Length)
        {
            throw new InvalidDataException("Composite profile has an invalid channel role list");
        }

        var index = 0;
        foreach (var role in roles.EnumerateArray())
        {
            if (role.GetString() != ExpectedRoles[index])
                throw new InvalidDataException($"Composite profile channel {index + 1} must be '{ExpectedRoles[index]}'");
            index++;
        }
    }

    private static void ValidateSpeakerControl(JsonElement root)
    {
        var controls = root.GetProperty("usbConfiguration").GetProperty("audioControls");
        foreach (var control in controls.EnumerateArray())
        {
            if (control.GetProperty("function").GetString() != "speaker")
                continue;

            var minimum = control.GetProperty("volumeMinRaw").GetInt32();
            var maximum = control.GetProperty("volumeMaxRaw").GetInt32();
            var current = control.GetProperty("volumeCurRaw").GetInt32();
            if (control.GetProperty("muteCur").GetInt32() != 0 ||
                current <= minimum || current > maximum)
            {
                throw new InvalidDataException("Composite profile speaker output must start audible and unmuted");
            }
            return;
        }

        throw new InvalidDataException("Composite profile has no speaker audio control");
    }

    private static void RequireString(JsonElement element, string property, string expected)
    {
        if (!element.TryGetProperty(property, out var value) || value.GetString() != expected)
            throw new InvalidDataException($"Composite profile '{property}' must be '{expected}'");
    }

    private static void RequireInt(JsonElement element, string property, int expected)
    {
        if (!element.TryGetProperty(property, out var value) ||
            !value.TryGetInt32(out var actual) || actual != expected)
        {
            throw new InvalidDataException($"Composite profile '{property}' must be {expected}");
        }
    }
}
