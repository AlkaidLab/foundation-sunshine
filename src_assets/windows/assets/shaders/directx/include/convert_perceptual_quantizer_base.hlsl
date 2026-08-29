#include "include/common.hlsl"
#include "include/hdr_pre_encode_transform.hlsl"

float3 ConvertScRGBTo2100PQ(float3 rgb)
{
    // PQ itself is absolute, so never scale the encoded PQ signal globally.
    return scRGBTo2100PQ(ApplyHdrPreEncodeTransform(rgb));
}

#define CONVERT_FUNCTION ConvertScRGBTo2100PQ
