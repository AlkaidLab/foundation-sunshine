/**
 * @file src/clipboard_wire.h
 * @brief Codec for the clipboard wire frames exchanged with the user-session
 *        GUI agent and the streaming client.
 *
 * The frame layout and the out-of-band descriptor are defined by the Rust
 * agent (sunshine-control-panel, src-tauri/src/clipboard.rs: `encode_frame`,
 * `decode_frame`, `RefMeta`); this header is the C++ mirror so every C++ peer
 * parses and builds them identically. The constants themselves live in
 * clipboard_bridge.h.
 *
 * A frame is `version, kind, token (LE32), length (LE32), payload`. A payload
 * larger than kInlineThresholdBytes travels out of band: it is stored in the
 * blob store and a kKindRef frame carries a JSON descriptor
 * (`{"id":..,"mime":..,"size":..}`) instead.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "clipboard_bridge.h"

namespace clipboard_wire {
  using payload_t = clipboard_bridge::payload_t;

  /// Decoded frame header, validated against the wire contract.
  struct header_t {
    std::uint8_t kind = 0;
    std::uint32_t token = 0;
    std::uint32_t length = 0;
  };

  inline std::uint32_t
  read_le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
  }

  inline bool
  is_known_kind(std::uint8_t kind) {
    return kind == clipboard_bridge::kKindText ||
           kind == clipboard_bridge::kKindPng ||
           kind == clipboard_bridge::kKindRef ||
           kind == clipboard_bridge::kKindFileOffer;
  }

  /**
   * @brief Validate and decode a frame header.
   * @returns nullopt for a short frame, an unknown version, an unknown kind, or
   *          a declared payload that the buffer does not actually hold - the
   *          same rejections the agent's decode_frame() makes.
   */
  inline std::optional<header_t>
  parse_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < clipboard_bridge::kFrameHeaderBytes || bytes[0] != clipboard_bridge::kWireVersion) {
      return std::nullopt;
    }

    header_t header;
    header.kind = bytes[1];
    if (!is_known_kind(header.kind)) {
      return std::nullopt;
    }
    header.token = read_le32(bytes, 2);
    header.length = read_le32(bytes, 6);
    if (bytes.size() < clipboard_bridge::kFrameHeaderBytes + static_cast<std::size_t>(header.length)) {
      return std::nullopt;
    }

    return header;
  }

  /// The frame payload, valid only after parse_header() accepted the buffer.
  inline std::span<const std::uint8_t>
  payload_of(std::span<const std::uint8_t> bytes, const header_t &header) {
    return bytes.subspan(clipboard_bridge::kFrameHeaderBytes, header.length);
  }

  /**
   * @brief Build one frame. `token` stays 0 for single-flavor changes; the
   *        agent reserves non-zero tokens for compound bursts.
   */
  inline payload_t
  encode(std::uint8_t kind, std::uint32_t token, std::span<const std::uint8_t> payload) {
    payload_t frame;
    frame.reserve(clipboard_bridge::kFrameHeaderBytes + payload.size());
    frame.push_back(clipboard_bridge::kWireVersion);
    frame.push_back(kind);
    for (int i = 0; i < 4; ++i) {
      frame.push_back(static_cast<std::uint8_t>((token >> (8 * i)) & 0xFF));
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
      frame.push_back(static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
  }

  inline payload_t
  encode_text(std::string_view text) {
    return encode(
      clipboard_bridge::kKindText, 0,
      std::span<const std::uint8_t> { reinterpret_cast<const std::uint8_t *>(text.data()), text.size() });
  }

  /// Descriptor carried by a kKindRef frame.
  struct ref_descriptor_t {
    std::string id;
    std::string mime;
    std::uint64_t size = 0;
  };

  /**
   * @brief Parse a kKindRef descriptor.
   * @returns nullopt for invalid JSON, a missing/oversized id, or a descriptor
   *          whose id is not a string; a missing mime or size is tolerated the
   *          way the agent tolerates them (the mime decides applicability
   *          later).
   */
  inline std::optional<ref_descriptor_t>
  parse_ref_descriptor(std::string_view json_text) {
    nlohmann::json doc;
    try {
      doc = nlohmann::json::parse(json_text);
    }
    catch (const std::exception &) {
      return std::nullopt;
    }

    if (!doc.is_object()) {
      return std::nullopt;
    }

    const auto id_it = doc.find("id");
    if (id_it == doc.end() || !id_it->is_string()) {
      return std::nullopt;
    }

    ref_descriptor_t descriptor;
    descriptor.id = id_it->get<std::string>();
    if (descriptor.id.empty() || descriptor.id.size() > clipboard_bridge::kMaxRefIdLength) {
      return std::nullopt;
    }

    if (const auto mime_it = doc.find("mime"); mime_it != doc.end() && mime_it->is_string()) {
      descriptor.mime = mime_it->get<std::string>();
    }
    if (const auto size_it = doc.find("size"); size_it != doc.end() && size_it->is_number_unsigned()) {
      descriptor.size = size_it->get<std::uint64_t>();
    }

    return descriptor;
  }

  /// Build the descriptor frame for a stored blob.
  inline payload_t
  encode_ref(std::string_view id, std::string_view mime, std::uint64_t size) {
    const auto descriptor = nlohmann::json {
      { "id", id },
      { "mime", mime },
      { "size", size },
    }.dump();
    return encode(
      clipboard_bridge::kKindRef, 0,
      std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t *>(descriptor.data()), descriptor.size() });
  }

  /// Whether a mime is text this host provider can apply.
  inline bool
  is_text_mime(std::string_view mime) {
    return mime.rfind("text/", 0) == 0;
  }
}  // namespace clipboard_wire
