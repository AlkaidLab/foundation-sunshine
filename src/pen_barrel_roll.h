/**
 * @file src/pen_barrel_roll.h
 * @brief Wire layout for voidlink-c's independent pen barrel roll extension.
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <moonlight-common-c/src/Input.h>

namespace input::pen_wire {
  constexpr std::uint32_t magic = 0x5500000B;
  static_assert(magic != SS_TOUCHPAD_MAGIC && magic != SS_TOUCHPAD_FRAME_MAGIC);
  constexpr std::uint16_t unknown = 0xffff;

  // Default to legacy azimuth until this session demonstrates real roll changes.
  class roll_probe_t {
  public:
    void reset() {
      first_ = unknown;
      varied_ = false;
    }

    std::uint16_t select(std::uint16_t roll, std::uint16_t azimuth) {
      if (roll == unknown) {
        return azimuth;
      }
      if (first_ == unknown) {
        first_ = roll;
      }
      if (roll != first_) {
        varied_ = true;
      }
      return varied_ ? roll : azimuth;
    }

  private:
    std::uint16_t first_ = unknown;
    bool varied_ = false;
  };

  // Mirrors SS_PEN_BARREL_ROLL_PACKET in voidlink-c, retaining the legacy
  // prefix and the two-byte trailing reserved field without changing it.
#pragma pack(push, 1)
  struct packet_t {
    SS_PEN_PACKET pen;
    std::uint16_t barrelRoll;
    std::uint16_t reserved;
  };
#pragma pack(pop)

  static_assert(sizeof(SS_PEN_PACKET) == 36);
  static_assert(offsetof(packet_t, barrelRoll) == 36);
  static_assert(sizeof(packet_t) == 40);

  constexpr std::uint16_t
  decode_roll(std::uint16_t value) {
    if constexpr (std::endian::native == std::endian::big) {
      value = std::byteswap(value);
    }
    return value == unknown ? unknown : value % 360;
  }

  constexpr bool
  valid_size(std::size_t available, std::uint32_t network_size) {
    if constexpr (std::endian::native == std::endian::little) {
      network_size = std::byteswap(network_size);
    }
    return available == sizeof(packet_t) && network_size == sizeof(packet_t) - sizeof(std::uint32_t);
  }
}  // namespace input::pen_wire
