// SDR NV12 CS converter: source is linear scRGB FP16, CONVERT_FUNCTION applies
// the sRGB OETF to land back in gamma space before the RGB->YCbCr matrix.
// Mirrors convert_yuv420_packed_uv_type0_ps_linear.hlsl.
#include "include/convert_linear_base.hlsl"

#include "include/convert_yuv420_nv12p010_cs_base.hlsl"
