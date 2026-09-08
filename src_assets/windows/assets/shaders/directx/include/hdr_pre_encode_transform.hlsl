#ifndef SUNSHINE_HDR_PRE_ENCODE_TRANSFORM_HLSL
#define SUNSHINE_HDR_PRE_ENCODE_TRANSFORM_HLSL

// Keep this layout in sync with HdrPreEncodeParams in display_vram.cpp.
cbuffer hdr_pre_encode_cbuffer : register(b3) {
    float hdr_nominal_peak_nits;
    float hdr_hlg_system_gamma;
    float hdr_sdr_band_gain;
    float hdr_sdr_band_top_scrgb;
};

// This is the single contract for transforms that change the linear scRGB
// pixels before PQ/HLG encoding. Luminance analysis must call this same
// function so dynamic metadata always describes the pixels actually encoded.
float3 ApplyHdrPreEncodeTransform(float3 rgb)
{
    // Re-anchor SDR-referenced content composed at the host's Windows SDR
    // white to the client's SDR reference white. Fade the gain in log-luminance
    // space so the mapping stays strictly monotonic before returning to unity.
    //
    // With r = max(2, gain^2), the minimum logarithmic slope for gain > 1 is:
    //   d(log(output)) / d(log(input))
    //     = 1 - log(gain) * smoothstep'(t) / log(r) >= 1 - 1.5 / 2 = 0.25
    // gain < 1 only increases that slope. This prevents the brightness reversal
    // produced by fading a large multiplicative gain over a fixed linear range.
    if (hdr_sdr_band_top_scrgb > 0.0 && hdr_sdr_band_gain != 1.0) {
        float level = max(rgb.r, max(rgb.g, rgb.b));
        float gain = max(hdr_sdr_band_gain, 1e-4);
        float fade_ratio = max(2.0, gain * gain);
        float level_ratio = max(level / hdr_sdr_band_top_scrgb, 1.0);
        float t = saturate(log2(level_ratio) / log2(fade_ratio));
        float gain_exponent = 1.0 - smoothstep(0.0, 1.0, t);
        rgb *= pow(gain, gain_exponent);
    }
    return rgb;
}

#define HDR_PRE_ENCODE_TRANSFORM_AVAILABLE 1

#endif
