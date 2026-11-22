// Linear FP16 to SDR NV12: apply the desktop transfer or HDR-to-SDR Method B
// before the RGB->YCbCr matrix.
// Mirrors convert_yuv420_packed_uv_type0_ps_linear.hlsl.
#include "include/convert_linear_base.hlsl"

#include "include/convert_yuv420_nv12p010_cs_base.hlsl"
