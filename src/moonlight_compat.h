/**
 * @file src/moonlight_compat.h
 * @brief Isolates C protocol macros that collide with Sunshine C++ symbols.
 */
#pragma once

// moonlight-common-c exposes these wire constants as C macros. Sunshine has
// strongly typed equivalents in hdr/dynamic_hdr_selection.h, so do not let the
// C names escape the protocol include boundary.
#undef DYNAMIC_HDR_CAPS_HDR10_PLUS
#undef DYNAMIC_HDR_CAPS_VIVID_PQ
#undef DYNAMIC_HDR_CAPS_VIVID_HLG
#undef DYNAMIC_HDR_CAPS_DOLBY_VISION_81
#undef DYNAMIC_HDR_CAPS_DOLBY_VISION_84
#undef DYNAMIC_HDR_FORMAT_NONE
#undef DYNAMIC_HDR_FORMAT_HDR10_PLUS
#undef DYNAMIC_HDR_FORMAT_VIVID_PQ
#undef DYNAMIC_HDR_FORMAT_VIVID_HLG
#undef DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_81
#undef DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_84
#undef DYNAMIC_HDR_FALLBACK_NONE
#undef DYNAMIC_HDR_FALLBACK_CODEC_UNSUPPORTED
#undef DYNAMIC_HDR_FALLBACK_COLORSPACE_UNSUPPORTED
#undef DYNAMIC_HDR_FALLBACK_CLIENT_CAPS_MISSING
#undef DYNAMIC_HDR_FALLBACK_DIRECT_SURFACE_MISSING
#undef DYNAMIC_HDR_FALLBACK_PREFERENCE
