/**
 * @file tests/tools/ds5_matrix/sim_rig.h
 * @brief Host-portable rig: the native feedback path rebuilt from the same inline functions.
 *
 * The Windows native client keeps its report builder and output-report dispatcher inside a class that
 * only exists on Windows.  This rig calls the same functions in the same order (tuning_runtime.h,
 * tuning_trigger.h, control_extension.h, button_map.h and the plain-C report core) so the matrix runs
 * on any host in seconds.  It is a model of that dispatcher, not the device: the Windows rig is the
 * authority and the two are compared row by row.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#include "alkaidlab/ds5_core/ds5_core.h"
#include "src/ds5/button_map.h"
#include "src/ds5/control_extension.h"
#include "src/ds5/rumble_policy.h"
#include "src/ds5/tuning_profile.h"
#include "src/ds5/tuning_runtime.h"
#include "src/ds5/tuning_trigger.h"
#include "tests/tools/ds5_matrix/matrix_rig.h"

namespace ds5::matrix {
  class sim_rig_t: public rig_t {
  public:
    explicit sim_rig_t(const std::filesystem::path &profile):
        path_(profile) {
      clear_store();
      report_.fill(0);
      tuning::runtime::state().active_controllers.fetch_add(1);
    }

    ~sim_rig_t() override { tuning::runtime::state().active_controllers.fetch_sub(1); }

    const char *
    name() const override { return "mirror"; }

    void
    clear_store() override {
      std::filesystem::remove(path_);
      store_ = std::make_unique<tuning::store_t>(path_);
      store_->load(nullptr);
      store_->publish();
    }

    post_result_t
    post(const json &values) override {
      auto result = store_->apply_json({ { "schema", "ALK-DS5T/1" }, { "values", values } });
      pushed_.clear();
      if (result.ok) {
        const auto before = pending_.size();
        push_saved_state();
        for (auto i = before; i < pending_.size(); ++i) {
          pushed_.push_back(pending_[i]);
          absorb(pending_[i]);
        }
      }
      return { result.ok, result.http_status, result.body.is_object() ? result.body : json { { "error", result.error } } };
    }

    json
    get() override { return store_->snapshot_json(); }

    std::vector<fb_t>
    output(const report48 &report) override {
      std::vector<fb_t> out = std::move(pending_);
      pending_.clear();
      for (const auto &queued : out) absorb(queued);
      const std::uint8_t *payload = report.data();
      AlkDs5Feedback value { sizeof(value), ALK_DS5_CORE_ABI_VERSION };
      if (alk_ds5_parse_output_report_ex(payload, report.size(), &value) != ALK_DS5_OK) return out;
      std::array<std::uint8_t, 8> state_to_send {};
      bool send_state = false;
      const auto trigger_config = tuning::trigger::read_config();
      tuning::trigger::plan_t plan;
      std::uint16_t left = static_cast<std::uint16_t>(value.motor_left) * 257u;
      std::uint16_t right = static_cast<std::uint16_t>(value.motor_right) * 257u;
      const auto *effect = payload + 1;
      const auto previous = haptics_state_;
      haptics_state_[0] = static_cast<std::uint8_t>(effect[0] & 3u);
      haptics_state_[1] = static_cast<std::uint8_t>(effect[38] & 4u);
      if ((haptics_state_[0] & 1u) != 0u || (haptics_state_[1] & 4u) != 0u) {
        haptics_state_[3] = effect[2];
        haptics_state_[4] = effect[3];
      }
      const auto merge = [&](std::uint8_t valid, std::size_t source, std::size_t target, std::uint8_t mask) {
        if ((effect[1] & valid) != 0u) {
          haptics_state_[2] = static_cast<std::uint8_t>(haptics_state_[2] | valid);
          haptics_state_[target] = static_cast<std::uint8_t>(effect[source] & mask);
        }
      };
      merge(0x02u, 9u, 5u, 0xffu);
      merge(0x40u, 36u, 6u, 0xffu);
      merge(0x20u, 39u, 7u, 0x01u);
      if (!state_initialized_ || previous != haptics_state_) {
        state_initialized_ = true;
        state_to_send = haptics_state_;
        send_state = true;
      }
      plan = tracker_.plan(payload, value.valid_flag0, trigger_config);
      if (state_initialized_ && ((haptics_state_[0] & 1u) != 0u || (haptics_state_[1] & 4u) != 0u)) {
        left = static_cast<std::uint16_t>(haptics_state_[4]) * 257u;
        right = static_cast<std::uint16_t>(haptics_state_[3]) * 257u;
      }
      if (send_state) {
        fb_t message { fb_kind::state };
        message.state = state_to_send;
        out.push_back(message);
      }
      const auto decision = rumble_tracker_.decide(ds5::rumble::from_body(effect));
      if (decision.raise) {
        held_rumble_ = { decision.left, decision.right };
        left = static_cast<std::uint16_t>(decision.left) * 257u;
        right = static_cast<std::uint16_t>(decision.right) * 257u;
        if ((decision.left | decision.right) != 0u) tuning::runtime::apply_rumble(left, right);
        fb_t message { fb_kind::rumble };
        message.low = left;
        message.high = right;
        out.push_back(message);
      }
      if (plan.flags != 0u) {
        std::array<std::uint8_t, 10> left_parameters {};
        std::array<std::uint8_t, 10> right_parameters {};
        std::copy_n(plan.left.data() + 1, 10, left_parameters.begin());
        std::copy_n(plan.right.data() + 1, 10, right_parameters.begin());
        tuning::runtime::apply_compat_triggers(plan.left[0], left_parameters.data(), plan.right[0], right_parameters.data());
        fb_t message { fb_kind::trigger };
        message.flags = plan.flags;
        message.left_type = plan.left[0];
        message.right_type = plan.right[0];
        message.left = left_parameters;
        message.right = right_parameters;
        out.push_back(message);
        if (plan.clear) {
          auto clear_left = plan.left;
          auto clear_right = plan.right;
          if (plan.clear_left) clear_left = {};
          if (plan.clear_right) clear_right = {};
          fb_t second { fb_kind::trigger };
          second.flags = plan.flags;
          second.left_type = clear_left[0];
          second.right_type = clear_right[0];
          std::copy_n(clear_left.data() + 1, 10, second.left.begin());
          std::copy_n(clear_right.data() + 1, 10, second.right.begin());
          out.push_back(second);
        }
      }
      if ((value.valid_flag1 & 4u) != 0u) {
        std::uint8_t red {}, green {}, blue {};
        tuning::runtime::apply_lightbar(red, green, blue, value.lightbar_rgb[0], value.lightbar_rgb[1], value.lightbar_rgb[2]);
        fb_t message { fb_kind::rgb };
        message.r = red;
        message.g = green;
        message.b = blue;
        out.push_back(message);
        lightbar_out_ = { red, green, blue };
        game_light_ = { value.lightbar_rgb[0], value.lightbar_rgb[1], value.lightbar_rgb[2] };
        have_game_light_ = true;
        light_sent_ = true;
      }
      auto current = control_extension::from_output_report(effect, 0);
      control_extension::drop_unset_player_leds(current, sent_extension_);
      control_extension::merge_fields(game_extension_, current);
      append_extension_changes(out);
      for (const auto &message : out) absorb(message);
      return out;
    }

    report64
    input(const pad_state_t &state) override {
      auto lt = state.lt, rt = state.rt;
      auto lsx = state.lsx, lsy = state.lsy, rsx = state.rsx, rsy = state.rsy;
      const auto triggers = tuning::runtime::shape_triggers(lt, rt);
      lt = triggers.left;
      rt = triggers.right;
      tuning::runtime::apply_stick(lsx, lsy, true);
      tuning::runtime::apply_stick(rsx, rsy, false);
      AlkDs5State value { sizeof(value), ALK_DS5_CORE_ABI_VERSION, 0, ++sequence_, 0, 8, lt, rt, 0,
        button_map::stick_x(lsx), button_map::stick_y(lsy), button_map::stick_x(rsx), button_map::stick_y(rsy), 0 };
      value.hat = button_map::hat(state.buttons);
      value.buttons = button_map::buttons(state.buttons, 0, 0) | (triggers.l2_pressed ? button_map::bit::l2 : 0u) |
                      (triggers.r2_pressed ? button_map::bit::r2 : 0u);
      std::array<std::uint8_t, 48> keep {};
      std::copy_n(report_.begin() + 16, keep.size(), keep.begin());
      std::size_t length = 0;
      if (alk_ds5_build_input_report(&value, report_.data(), report_.size(), &length) == ALK_DS5_OK) {
        std::copy(keep.begin(), keep.end(), report_.begin() + 16);
        report_[7] = static_cast<std::uint8_t>(value.sequence);
      }
      return report_;
    }

    report64
    gyro(float x, float y, float z, bool read) override {
      if (!tuning::runtime::apply_gyro(x, y, z, filter_)) {
        x = y = z = 0.0f;
      }
      const auto to_counts = [](float v) {
        return static_cast<std::int16_t>(std::clamp(std::lround(v * 16.4f), -32768L, 32767L));
      };
      put(16, to_counts(x));
      put(18, to_counts(y));
      put(20, to_counts(z));
      return read ? report_ : report64 {};
    }

    report64
    touch(int event, std::uint32_t pointer, float x, float y) override {
      const auto offset = pointer & 1u ? 37u : 33u;
      const auto px = static_cast<std::uint16_t>(x * 1920.f);
      const auto py = static_cast<std::uint16_t>(y * 943.f);
      report_[offset] = static_cast<std::uint8_t>((event == 2 ? 0x80u : 0) | (pointer & 0x7fu));
      report_[offset + 1] = static_cast<std::uint8_t>(px);
      report_[offset + 2] = static_cast<std::uint8_t>((px >> 8) | (py << 4));
      report_[offset + 3] = static_cast<std::uint8_t>(py >> 4);
      return report_;
    }

    std::vector<fb_t>
    play(std::span<const std::int16_t> lanes) override {
      std::vector<fb_t> out;
      if (!tuning::runtime::allow_authored_pcm() || !tuning::runtime::pcm_source_enabled(true)) return out;
      const auto total = lanes.size() / 2;
      for (std::size_t offset = 0; offset < total;) {
        const auto frames = std::min<std::size_t>(240, total - offset);
        std::vector<std::uint8_t> bytes(frames * 4);
        std::copy_n(reinterpret_cast<const std::uint8_t *>(lanes.data() + offset * 2), frames * 4, bytes.data());
        tuning::runtime::apply_pcm_gain(bytes.data(), bytes.size());
        fb_t message { fb_kind::pcm };
        message.flags = pcm_started_ ? 0u : 1u;
        pcm_started_ = true;
        message.pcm.resize(frames * 2);
        std::copy_n(reinterpret_cast<const std::int16_t *>(bytes.data()), frames * 2, message.pcm.begin());
        out.push_back(std::move(message));
        offset += frames;
      }
      return out;
    }

    std::vector<fb_t>
    end_session() override {
      std::vector<fb_t> out;
      while (tuning::runtime::state().release_on_end.load() && (lightbar_out_[0] | lightbar_out_[1] | lightbar_out_[2]) != 0) {
        for (auto &channel : lightbar_out_) channel = tuning::runtime::fade_channel(channel);
        fb_t message { fb_kind::rgb };
        message.r = lightbar_out_[0];
        message.g = lightbar_out_[1];
        message.b = lightbar_out_[2];
        out.push_back(message);
      }
      pcm_started_ = false;
      return out;
    }

    std::uint32_t
    identity_mode() override { return tuning::runtime::identity_gamepad_mode(); }

  private:
    void
    put(std::size_t at, std::int16_t v) {
      report_[at] = static_cast<std::uint8_t>(v & 0xff);
      report_[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    }

    void
    append_extension_changes(std::vector<fb_t> &out) {
      const auto delta = tuning::runtime::plan_extension_update(game_extension_, sent_extension_, 0, (jack_bits_ & 0x01u) != 0u);
      if (delta.valid_mask == 0u) return;
      fb_t message { fb_kind::ext };
      message.mask = delta.valid_mask;
      message.player = delta.player_led_mask;
      message.mute = delta.mute_led;
      message.speaker = delta.speaker_volume;
      message.headset = delta.headphone_volume;
      message.mic = delta.mic_volume;
      message.mic_mute = delta.mic_mute;
      message.route = delta.audio_routing;
      message.brightness = delta.lightbar_brightness;
      out.push_back(message);
    }

    /** @brief What the host does after a tuning save: the same push as native_client_t::push_tuning_state. */
    void
    push_saved_state() {
      append_extension_changes(pending_);
      /* The game's last trigger effects go through the saved settings again, and a steady rumble is shaped again. */
      const auto trigger_plan = tracker_.plan(nullptr, 0u, tuning::trigger::read_config());
      if (trigger_plan.flags != 0u) {
        std::array<std::uint8_t, 10> left_parameters {};
        std::array<std::uint8_t, 10> right_parameters {};
        std::copy_n(trigger_plan.left.data() + 1, 10, left_parameters.begin());
        std::copy_n(trigger_plan.right.data() + 1, 10, right_parameters.begin());
        auto left_type = trigger_plan.left[0];
        auto right_type = trigger_plan.right[0];
        tuning::runtime::apply_compat_triggers(left_type, left_parameters.data(), right_type, right_parameters.data());
        fb_t message { fb_kind::trigger };
        message.flags = trigger_plan.flags;
        message.left_type = left_type;
        message.right_type = right_type;
        message.left = left_parameters;
        message.right = right_parameters;
        pending_.push_back(message);
      }
      if (rumble_tracker_.running() && (held_rumble_[0] | held_rumble_[1]) != 0u) {
        auto left = static_cast<std::uint16_t>(held_rumble_[0] * 257u);
        auto right = static_cast<std::uint16_t>(held_rumble_[1] * 257u);
        tuning::runtime::apply_rumble(left, right);
        fb_t message { fb_kind::rumble };
        message.low = left;
        message.high = right;
        pending_.push_back(message);
      }
      const auto push = tuning::runtime::plan_lightbar_push(have_game_light_, game_light_, lightbar_out_, light_sent_);
      if (push.emit) {
        fb_t message { fb_kind::rgb };
        message.r = push.red;
        message.g = push.green;
        message.b = push.blue;
        pending_.push_back(message);
        lightbar_out_ = { push.red, push.green, push.blue };
        light_sent_ = true;
      }
    }

    std::filesystem::path path_;
    std::unique_ptr<tuning::store_t> store_;
    report64 report_ {};
    std::uint64_t sequence_ { 1 };
    std::array<std::uint8_t, 8> haptics_state_ {};
    bool state_initialized_ { false };
    bool pcm_started_ { false };
    std::uint8_t jack_bits_ { 0 };
    std::array<std::uint8_t, 3> lightbar_out_ {};
    tuning::trigger::tracker_t tracker_;
    control_extension::output_t sent_extension_ {};
    control_extension::output_t game_extension_ {};
    ds5::rumble::tracker_t rumble_tracker_;
    std::array<std::uint8_t, 2> held_rumble_ {};
    std::array<std::uint8_t, 3> game_light_ {};
    bool have_game_light_ { false };
    bool light_sent_ { false };
    std::vector<fb_t> pending_;
    tuning::runtime::motion_filter_state_t filter_;
  };
}  // namespace ds5::matrix
