using Windows.Win32;
using Windows.Win32.Media.Audio;

namespace Sunshine.Ds5Sidecar;

/// <summary>Applies and verifies Windows' three quadraphonic speaker settings.</summary>
internal static class DualSenseSpeakerConfiguration
{
    private const uint QuadraphonicSpeakerMask = 0x00000033;
    private static readonly Guid PcmSubFormat = new("00000001-0000-0010-8000-00AA00389B71");

    internal static bool IsConfigured(string endpointId)
    {
        using var policy = new AudioPolicyConfig();
        return ReadAndValidate(policy, endpointId);
    }

    internal static bool Ensure(TimeSpan timeout, out bool changed)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            var endpoints = DefaultAudioEndpointGuard.GetActiveVirtualDualSenseRenderEndpoints();
            if (endpoints.Count != 0)
            {
                changed = false;
                using var policy = new AudioPolicyConfig();
                foreach (var endpoint in endpoints)
                {
                    changed |= !ReadAndValidate(policy, endpoint);
                    policy.SetFormat(endpoint, CreateQuadraphonicFormat());
                    policy.SetUInt32(endpoint, PInvoke.PKEY_AudioEndpoint_PhysicalSpeakers, QuadraphonicSpeakerMask);
                    policy.SetUInt32(endpoint, PInvoke.PKEY_AudioEndpoint_FullRangeSpeakers, QuadraphonicSpeakerMask);
                    if (!ReadAndValidate(policy, endpoint))
                        throw new InvalidOperationException("Windows did not retain the DualSense speaker configuration");
                }
                return true;
            }
            if (DateTime.UtcNow >= deadline)
                break;
            Thread.Sleep(50);
        }
        while (true);
        changed = false;
        return false;
    }

    private static bool ReadAndValidate(AudioPolicyConfig policy, string endpointId) =>
        policy.ReadUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_PhysicalSpeakers) == QuadraphonicSpeakerMask &&
        policy.ReadUInt32(endpointId, PInvoke.PKEY_AudioEndpoint_FullRangeSpeakers) == QuadraphonicSpeakerMask &&
        IsQuadraphonic(policy.ReadFormat(endpointId));

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

    internal static bool IsQuadraphonic(WAVEFORMATEXTENSIBLE format) =>
        format.Format.wFormatTag == 0xFFFE && format.Format.nChannels == 4 &&
        format.Format.nSamplesPerSec == 48000 && format.Format.wBitsPerSample == 16 &&
        format.dwChannelMask == QuadraphonicSpeakerMask && format.SubFormat == PcmSubFormat;
}
