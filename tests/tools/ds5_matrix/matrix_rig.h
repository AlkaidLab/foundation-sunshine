/**
 * @file tests/tools/ds5_matrix/matrix_rig.h
 * @brief Neutral types and the device interface of the DualSense tuning behaviour matrix.
 *
 * The matrix probes only talk to a rig_t.  One rig mirrors the native feedback path with the same
 * inline functions on any host; the other drives the real virtual DualSense on Windows.  Messages
 * are normalised here so a probe reads the same fields on both.
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ds5::matrix {
  using json = nlohmann::json;
  using report64 = std::array<std::uint8_t, 64>;
  using report48 = std::array<std::uint8_t, 48>;

  /** @brief Gamepad state as the stream layer submits it (client button flag bits). */
  struct pad_state_t {
    std::uint32_t buttons {};
    std::uint8_t lt {};
    std::uint8_t rt {};
    std::int16_t lsx {};
    std::int16_t lsy {};
    std::int16_t rsx {};
    std::int16_t rsy {};
  };

  enum class fb_kind { rumble,
    trigger,
    rgb,
    ext,
    state,
    pcm };

  /** @brief One feedback message as it leaves the host path towards the client. */
  struct fb_t {
    fb_kind kind {};
    std::uint16_t low {};  ///< rumble low-frequency motor
    std::uint16_t high {};  ///< rumble high-frequency motor
    std::uint8_t flags {};  ///< trigger sides (0x08 left, 0x04 right) or pcm stream flags
    std::uint8_t left_type {};
    std::uint8_t right_type {};
    std::array<std::uint8_t, 10> left {};
    std::array<std::uint8_t, 10> right {};
    std::uint8_t r {};
    std::uint8_t g {};
    std::uint8_t b {};
    std::uint32_t mask {};  ///< output extension valid mask
    std::uint8_t player {};
    std::uint8_t mute {};
    std::uint8_t speaker {};
    std::uint8_t headset {};
    std::uint8_t mic {};
    std::uint8_t mic_mute {};
    std::uint8_t route {};
    std::uint8_t brightness {};
    std::array<std::uint8_t, 8> state {};
    std::vector<std::int16_t> pcm;  ///< interleaved two-lane frames
  };

  /** @brief The output extension as the client holds it after every delta so far. */
  struct ext_view_t {
    std::uint8_t player {};
    std::uint8_t mute {};
    std::uint8_t speaker {};
    std::uint8_t headset {};
    std::uint8_t mic {};
    std::uint8_t mic_mute {};
    std::uint8_t route {};
    std::uint8_t brightness {};
  };

  /** @brief The adaptive-trigger state the client holds after every message so far. */
  struct trigger_view_t {
    std::uint8_t left_type {};
    std::uint8_t right_type {};
    std::array<std::uint8_t, 10> left {};
    std::array<std::uint8_t, 10> right {};
  };

  struct post_result_t {
    bool ok {};
    int http_status {};
    json body;
  };

  class rig_t {
  public:
    virtual ~rig_t() = default;
    virtual const char *
    name() const = 0;
    /** @brief The saved-value path of the settings page: validate, persist, publish. */
    virtual post_result_t
    post(const json &values) = 0;
    /** @brief Forget every saved value (the profile file is removed) and publish the defaults. */
    virtual void
    clear_store() = 0;
    /** @brief GET snapshot of the same store. */
    virtual json
    get() = 0;
    /** @brief Write one 48-byte output report as a game does and return what leaves for the client. */
    virtual std::vector<fb_t>
    output(const report48 &report) = 0;
    /** @brief Submit one gamepad state and return the input report a game reads next. */
    virtual report64
    input(const pad_state_t &state) = 0;
    /** @brief Submit one gyro sample (deg/s); return the report only when @p read is set. */
    virtual report64
    gyro(float x, float y, float z, bool read) = 0;
    /** @brief Submit one touch contact (0 down, 1 move, 2 up) and return the input report. */
    virtual report64
    touch(int event, std::uint32_t pointer, float x, float y) = 0;
    /** @brief Render interleaved two-lane haptics PCM at 48 kHz to the pad and return the PCM messages. */
    virtual std::vector<fb_t>
    play(std::span<const std::int16_t> lanes) = 0;
    /** @brief End the session (lightbar release); returns the messages sent while ending. A new session follows. */
    virtual std::vector<fb_t>
    end_session() = 0;
    /** @brief The identity mode the next controller allocation would use. */
    virtual std::uint32_t
    identity_mode() = 0;

    /** @brief The messages the host raised by itself because of the last save (no game report involved). */
    const std::vector<fb_t> &
    pushed() const { return pushed_; }
    /** @brief How many times the pad was attached to the stream since the rig started; a live check needs 1. */
    virtual int
    attach_count() const { return 1; }

    const ext_view_t &
    ext_view() const { return ext_; }
    const trigger_view_t &
    trigger_view() const { return trigger_; }

  protected:
    void
    absorb(const fb_t &message) {
      if (message.kind == fb_kind::ext) {
        if (message.mask & 0x01u) ext_.player = message.player;
        if (message.mask & 0x02u) ext_.mute = message.mute;
        if (message.mask & 0x04u) ext_.speaker = message.speaker;
        if (message.mask & 0x08u) ext_.headset = message.headset;
        if (message.mask & 0x10u) ext_.mic = message.mic;
        if (message.mask & 0x20u) ext_.mic_mute = message.mic_mute;
        if (message.mask & 0x40u) ext_.route = message.route;
        if (message.mask & 0x80u) ext_.brightness = message.brightness;
      }
      else if (message.kind == fb_kind::trigger) {
        if (message.flags & 0x08u) {
          trigger_.left_type = message.left_type;
          trigger_.left = message.left;
        }
        if (message.flags & 0x04u) {
          trigger_.right_type = message.right_type;
          trigger_.right = message.right;
        }
      }
    }

  protected:
    std::vector<fb_t> pushed_;

  private:
    ext_view_t ext_ {};
    trigger_view_t trigger_ {};
  };
}  // namespace ds5::matrix
