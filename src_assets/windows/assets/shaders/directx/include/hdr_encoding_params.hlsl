#ifndef SUNSHINE_HDR_ENCODING_PARAMS_HLSL
#define SUNSHINE_HDR_ENCODING_PARAMS_HLSL

// Keep this layout in sync with HdrEncodingParams in display_vram.cpp.
cbuffer hdr_encoding_cbuffer : register(b3) {
    float hdr_nominal_peak_nits;
    float hdr_hlg_system_gamma;
    float2 hdr_encoding_padding;
};

#endif
