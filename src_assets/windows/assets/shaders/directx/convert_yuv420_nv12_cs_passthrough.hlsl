// SDR NV12 CS converter: source is sRGB gamma-encoded BGRA8 (passthrough).
// CONVERT_FUNCTION just clamps to [0,1] before the RGB->YCbCr matrix runs in
// gamma space, matching convert_yuv420_packed_uv_type0_ps.hlsl.
#include "include/convert_base.hlsl"

#include "include/convert_yuv420_nv12p010_cs_base.hlsl"
