using Windows.Win32;
using Windows.Win32.Media.Audio;

namespace Sunshine.Ds5Sidecar;

/// <summary>Applies and verifies Windows' three quadraphonic speaker settings.</summary>
internal static class DualSenseSpeakerConfiguration
{
    private const uint QuadraphonicSpeakerMask = 0x00000033;
    private static readonly Guid PcmSubFormat = new("00000001-0000-0010-8000-00AA00389B71");

    /// <summary>Reads only the caller-selected endpoint so another active endpoint cannot satisfy acceptance.</summary>
    internal static bool IsConfigured(string endpointId)
    {
        using var policy = new AudioPolicyConfig();
        return ReadAndValidate(policy, endpointId);
    }

    /// <summary>Configures only the current controller's endpoint and verifies all three speaker settings.</summary>
    internal static void Ensure(string endpointId, out bool changed)
    {
        using var policy = new AudioPolicyConfig();
        changed = !ReadAndValidate(policy, endpointId);
        policy.SetFormat(endpointId, CreateQuadraphonicFormat());
        policy.SetUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_PhysicalSpeakers, QuadraphonicSpeakerMask);
        policy.SetUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_FullRangeSpeakers, QuadraphonicSpeakerMask);
        if (!ReadAndValidate(policy, endpointId))
            throw new InvalidOperationException("Windows did not retain the DualSense speaker configuration");
    }

    /// <summary>Requires both speaker masks and the complete four-channel PCM format to agree.</summary>
    private static bool ReadAndValidate(AudioPolicyConfig policy, string endpointId) =>
        policy.ReadUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_PhysicalSpeakers) == QuadraphonicSpeakerMask &&
        policy.ReadUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_FullRangeSpeakers) == QuadraphonicSpeakerMask &&
        IsQuadraphonic(policy.ReadFormat(endpointId));

    /// <summary>Builds the 48 kHz, 16-bit quadraphonic format expected by the composite profile.</summary>
    internal static WAVEFORMATEXTENSIBLE CreateQuadraphonicFormat() => new()
    {
        Format = new WAVEFORMATEX
        {
            wFormatTag = 0xFFFE,
            nChannels = 4,
            nSamplesPerSec = 48000,
            nAvgBytesPerSec = 48000 * 4 * 2,
            nBlockAlign = 4 * 2,
            wBitsPerSample = 16,
            cbSize = 22,
        },
        Samples = new() { wValidBitsPerSample = 16 },
        dwChannelMask = QuadraphonicSpeakerMask,
        SubFormat = PcmSubFormat,
    };

    /// <summary>Rejects a stereo mask or a format inconsistent with the composite audio contract.</summary>
    internal static bool IsQuadraphonic(WAVEFORMATEXTENSIBLE format) =>
        format.Format.wFormatTag == 0xFFFE && format.Format.nChannels == 4 &&
        format.Format.nSamplesPerSec == 48000 && format.Format.wBitsPerSample == 16 &&
        format.dwChannelMask == QuadraphonicSpeakerMask && format.SubFormat == PcmSubFormat;
}
