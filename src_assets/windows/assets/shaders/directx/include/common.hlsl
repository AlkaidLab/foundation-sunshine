// This is a fast sRGB approximation from Microsoft's ColorSpaceUtility.hlsli
float3 ApplySRGBCurve(float3 x)
{
    float3 result;
    result.r = x.r < 0.0031308 ? 12.92 * x.r : 1.13005 * sqrt(x.r - 0.00228) - 0.13448 * x.r + 0.005719;
    result.g = x.g < 0.0031308 ? 12.92 * x.g : 1.13005 * sqrt(x.g - 0.00228) - 0.13448 * x.g + 0.005719;
    result.b = x.b < 0.0031308 ? 12.92 * x.b : 1.13005 * sqrt(x.b - 0.00228) - 0.13448 * x.b + 0.005719;
    return result;
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

// HLG (Hybrid Log-Gamma) OETF as defined in ARIB STD-B67 / ITU-R BT.2100.
float3 LinearToHLG(float3 L)
{
    // HLG constants from ARIB STD-B67
    static const float a = 0.17883277;
    static const float b = 0.28466892;  // 1 - 4 * a
    static const float c = 0.55991073;  // 0.5 - a * ln(4 * a)
    static const float threshold = 1.0 / 12.0;

    // Clamp negative values only. Production signals may retain headroom above 1.
    L = max(L, 0.0);

    // Compute both branches for all channels (branchless)
    // Low range: sqrt(3 * L)
    float3 lowRange = sqrt(3.0 * L);

    // High range: a * log(12 * L - b) + c
    float3 highRange = a * log(max(12.0 * L - b, 1e-6)) + c;

    // Branchless select using step function
    float3 selector = step(threshold, L);

    return lerp(lowRange, highRange, selector);
}

// Convert display-linear Rec. 2020 light to scene-linear HLG light using the
// inverse of the BT.2100 reference OOTF (Table 5, Note 5i).
float3 DisplayLinearToHLGScene(float3 display_rgb_nits, float peak_nits, float system_gamma)
{
    float3 normalized_rgb = max(display_rgb_nits, 0.0) / peak_nits;
    float normalized_luminance = dot(
        normalized_rgb,
        float3(0.2627, 0.6780, 0.0593)
    );

    // Avoid the 0 * infinity form when gamma > 1 and luminance is black.
    if (normalized_luminance <= 0.0) {
        return 0.0;
    }

    float ootf_scale = pow(
        max(normalized_luminance, 1e-6),
        (1.0 - system_gamma) / system_gamma
    );
    return normalized_rgb * ootf_scale;
}

float3 scRGBTo2100HLG(float3 rgb, float peak_nits, float system_gamma)
{
    // Windows capture supplies display-linear scRGB: Rec. 709 primaries and
    // 1.0 = 80 cd/m2. Convert primaries before applying the luminance-dependent
    // inverse OOTF so its Y calculation is in Rec. 2020.
    float3 display_rgb_nits = Rec709toRec2020(rgb) * 80.0;
    float3 scene_rgb = DisplayLinearToHLGScene(
        display_rgb_nits,
        peak_nits,
        system_gamma
    );
    return LinearToHLG(scene_rgb);
}
