#include "include/common.hlsl"

// Preserve captured absolute luminance. SDR and HDR pixels cannot be separated
// by brightness once they have been composed into the same scRGB frame.
#define CONVERT_FUNCTION scRGBTo2100PQ
