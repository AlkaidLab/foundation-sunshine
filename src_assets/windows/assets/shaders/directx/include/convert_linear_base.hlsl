#include "include/common.hlsl"
#include "include/hdr_encoding_params.hlsl"

// BT.2100 LMS matrix composed with the linear primary conversions.
static const float3x3 Rec709ToLms = {
    0.295809355, 0.623101305, 0.081089341,
    0.156248976, 0.727325725, 0.116425299,
    0.035135663, 0.156574727, 0.808289610
};
static const float3x3 LmsToRec709 = {
     6.173362742, -5.320380568,  0.147017826,
    -1.324311500,  2.560226421, -0.235914921,
    -0.011816903, -0.264672035,  1.276488938
};
static const float3x3 LmsToSmpteC = {
     6.650457157, -5.807419747,  0.156962590,
    -1.493468713,  2.762204921, -0.268736208,
    -0.007513590, -0.260458703,  1.267972293
};

float3 PqToSdrLinear(float3 signal)
{
    float3 p = pow(signal, 32.0 / 2523.0);
    return 100.0 * pow(max(p - 3424.0 / 4096.0, 0.0) /
                       (2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0);
}

// Section 5.2.4: reduce chroma at constant ICtCp intensity and hue until the
// colour intersects the target RGB cube. ICtCp is linear in PQ-encoded LMS:
// scaling Ct and Cp together is equivalent to interpolating from (I,I,I).
float3 LimitSdrGamut(float3 rgb709, float3 target, float3x3 lms_to_target)
{
    [branch] if (all(target >= 0.0) && all(target <= 1.0)) return target;
    if (all(target >= 1.0)) return 1.0;
    float3 lms_pq = NitsToPQ(mul(Rec709ToLms, rgb709) * 100.0);
    float intensity = (lms_pq.x + lms_pq.y) * 0.5;
    if (intensity >= 0.50807842) return 1.0; // PQ at 100 cd/m²
    float lo = 0.0, hi = 1.0;
    float3 boundary = target;
    [loop] for (int step = 0; step < 12; ++step) {
        float mid = (lo + hi) * 0.5;
        float3 candidate = mul(lms_to_target, PqToSdrLinear(lerp(intensity, lms_pq, mid)));
        if (all(candidate >= 0.0) && all(candidate <= 1.0)) {
            lo = mid;
        } else {
            hi = mid;
            boundary = candidate;
        }
    }
    return saturate(boundary);
}

// Report ITU-R BT.2446-1, Method B, section 5.2 and Fig. 6.
float3 MethodBScRgbToSdr(float3 rgb)
{
    // scRGB already has BT.709 primaries. Changing only Y at fixed u'v'
    // scales all linear RGB components equally, including negative components.
    float y = dot(rgb, float3(0.212639, 0.715169, 0.072192));
    if (y <= 0.0) return 0.0;

    // Invert the 1000-nit HLG reference OOTF (gamma 1.2), then render
    // the same scene light at 291 nits with gamma 1.0 (section 5.2.1).
    float bridge_y = 291.0 * pow(y * (80.0 / 1000.0), 1.0 / 1.2);
    // 78% BT.1886 signal is 55.0842 nits. Choose the width of the log
    // shoulder so it joins with unit slope and maps 291 to nominal SDR white.
    const float knee = 55.0842;
    const float width = 16.44899;
    float sdr_y = bridge_y <= knee ? bridge_y :
                  knee + width * log(1.0 + (bridge_y - knee) / width);
    sdr_y /= 100.0;

    float3 mapped = rgb * (sdr_y / y);
    float3 sdr = LimitSdrGamut(mapped, mapped, LmsToRec709);
    // Method B produces BT.709. Encode those colours in the stream's
    // negotiated primaries before the existing RGB -> Y'CbCr matrix.
    if (sdr_output_primaries == 6) { // H.273 SMPTE 170M, D65
        const float3x3 to_smpte_c = {
             1.065379, -0.055401, -0.009978,
            -0.019633,  1.036363, -0.016730,
             0.001632,  0.004412,  0.993956
        };
        sdr = LimitSdrGamut(sdr, mul(to_smpte_c, sdr), LmsToSmpteC);
    } else if (sdr_output_primaries == 9) { // H.273 BT.2020
        sdr = Rec709toRec2020(sdr);
    }
    // Inverse BT.1886, reference black 0, reference white 100 cd/m².
    return pow(sdr, 1.0 / 2.4);
}

float3 CONVERT_FUNCTION(float3 input)
{
    if (sdr_output_primaries != 0) return MethodBScRgbToSdr(input);
    return ApplySRGBCurve(saturate(input));
}
