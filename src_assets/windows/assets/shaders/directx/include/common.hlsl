// This is a fast sRGB approximation from Microsoft's ColorSpaceUtility.hlsli
float3 ApplySRGBCurve(float3 x)
{
    return x < 0.0031308 ? 12.92 * x : 1.13005 * sqrt(x - 0.00228) - 0.13448 * x + 0.005719;
}

float3 NitsToPQ(float3 L)
{
    // Constants from SMPTE 2084 PQ
    static const float m1 = 2610.0 / 4096.0 / 4;
    static const float m2 = 2523.0 / 4096.0 * 128;
    static const float c1 = 3424.0 / 4096.0;
    static const float c2 = 2413.0 / 4096.0 * 32;
    static const float c3 = 2392.0 / 4096.0 * 32;

    float3 Lp = pow(saturate(L / 10000.0), m1);
    return pow((c1 + c2 * Lp) / (1 + c3 * Lp), m2);
}

float3 Rec709toRec2020(float3 rec709)
{
    static const float3x3 ConvMat =
    {
        0.627402, 0.329292, 0.043306,
        0.069095, 0.919544, 0.011360,
        0.016394, 0.088028, 0.895578
    };
    return mul(ConvMat, rec709);
}

float3 scRGBTo2100PQ(float3 rgb)
{
    // Convert from Rec 709 primaries (used by scRGB) to Rec 2020 primaries (used by Rec 2100)
    rgb = Rec709toRec2020(rgb);

    // 1.0f is defined as 80 nits in the scRGB colorspace
    rgb *= 80;

    // Apply the PQ transfer function on the raw color values in nits
    return NitsToPQ(rgb);
}

// HLG (Hybrid Log-Gamma) OETF as defined in ARIB STD-B67 / ITU-R BT.2100
// Optimized: branchless vectorized implementation
//
// Note: HLG OETF is mathematically valid for L > 1, but output > 1 will be
// clipped by the encoder (10-bit can only represent [0, 1]). We don't clip
// in the shader to preserve precision; the encoder handles clipping.
float3 LinearToHLG(float3 L)
{
    // HLG constants from ARIB STD-B67
    static const float a = 0.17883277;
    static const float b = 0.28466892;  // 1 - 4 * a
    static const float c = 0.55991073;  // 0.5 - a * ln(4 * a)
    static const float threshold = 1.0 / 12.0;

    // Clamp negative values only (out of gamut), allow > 1 for HDR headroom
    L = max(L, 0.0);

    // Compute both branches for all channels (branchless)
    // Low range: sqrt(3 * L)
    float3 lowRange = sqrt(3.0 * L);

    // High range: a * log(12 * L - b) + c
    // For L > 1, this produces output > 1 which is fine (encoder clips later)
    float3 highRange = a * log(max(12.0 * L - b, 1e-6)) + c;

    // Branchless select using step function
    float3 selector = step(threshold, L);

    // Return unclamped result - let encoder handle clipping
    return lerp(lowRange, highRange, selector);
}

float3 scRGBTo2100HLG(float3 rgb)
{
    // Convert from Rec 709 primaries (used by scRGB) to Rec 2020 primaries (used by Rec 2100)
    rgb = Rec709toRec2020(rgb);

    // scRGB luminance mapping to HLG:
    // - scRGB 1.0 = 80 nits (SDR reference white)
    // - HLG is scene-referred, OETF expects normalized scene light [0, 1]
    // - For a 1000 nits peak display, HLG signal 1.0 maps to ~1000 nits
    // - HLG reference white (75% signal) is ~203 nits
    //
    // Mapping strategy:
    // - scRGB 1.0 (80 nits) should map to HLG ~0.5 (SDR-compatible level)
    // - scRGB 12.5 (1000 nits) should map to HLG 1.0 (peak white)
    //
    // Scale factor: 80 nits / 1000 nits = 0.08
    // This maps the full HDR range [0, 1000 nits] to HLG input [0, 1]
    
    static const float HDR_PEAK_NITS = 1000.0;
    static const float SCRGB_NITS_PER_UNIT = 80.0;
    static const float scaleToHLG = SCRGB_NITS_PER_UNIT / HDR_PEAK_NITS;  // 0.08
    
    // Convert scRGB to normalized scene light for HLG
    // Negative values are clamped (out of gamut), but > 1.0 is preserved
    // This allows content > 1000 nits to pass through (soft rolloff)
    rgb = max(rgb, 0.0) * scaleToHLG;

    // Apply the HLG OETF
    // For input > 1.0, output will exceed 1.0 and be clipped by encoder
    // This provides a natural "soft knee" rolloff for super-bright content
    return LinearToHLG(rgb);
}
