#include "googcc_adapter.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/transport/network_control.h"
#include "logging/rtc_event_log/events/rtc_event_probe_result_failure.h"
#include "logging/rtc_event_log/events/rtc_event_probe_result_success.h"
#include "modules/congestion_controller/goog_cc/goog_cc_network_control.h"
#include "system_wrappers/include/clock.h"

namespace transport {
  namespace {
    // Leave ample headroom for upstream timestamp arithmetic and NTP conversion.
    constexpr auto max_time_us = std::numeric_limits<std::int64_t>::max() / 4;

    // Observe the existing upstream decision; never calculate a second probe
    // estimate, retain packet history, or alter the controller's update.
    class probe_event_log_t final: public webrtc::RtcEventLog {
    public:
      probe_event_log_t(googcc_snapshot_t &state, webrtc::SimulatedClock &clock, std::int32_t probe_base):
          state_(state), clock_(clock), probe_base_(probe_base) {}
      bool
      StartLogging(std::unique_ptr<webrtc::RtcEventLogOutput>, std::int64_t) override { return false; }
      void
      StopLogging() override {}
      void
      Log(std::unique_ptr<webrtc::RtcEvent> event) override {
        if (event->GetType() == webrtc::RtcEventProbeResultSuccess::kType) {
          const auto &probe = static_cast<const webrtc::RtcEventProbeResultSuccess &>(*event);
          ++state_.native_probe_successes;
          state_.last_native_probe_cluster = physical_id(probe.id());
          state_.last_native_probe_bps = probe.bitrate_bps();
          state_.last_native_probe_failure = -1;
        }
        else if (event->GetType() == webrtc::RtcEventProbeResultFailure::kType) {
          const auto &probe = static_cast<const webrtc::RtcEventProbeResultFailure &>(*event);
          ++state_.native_probe_failures;
          state_.last_native_probe_cluster = physical_id(probe.id());
          state_.last_native_probe_bps = 0;
          state_.last_native_probe_failure = static_cast<std::int32_t>(probe.failure_reason());
        }
        else
          return;
        state_.last_native_probe_at_us = clock_.TimeInMicroseconds();
      }

    private:
      std::int32_t
      physical_id(std::int32_t native) const {
        const auto physical = static_cast<std::int64_t>(native) + probe_base_;
        if (native < 0 || physical > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Probe identity exhausted");
        return static_cast<std::int32_t>(physical);
      }
      googcc_snapshot_t &state_;
      webrtc::SimulatedClock &clock_;
      const std::int32_t probe_base_;
    };

    webrtc::Environment
    make_environment(googcc_snapshot_t &state, webrtc::SimulatedClock *&clock, const googcc_config_t &config) {
      auto owned_clock = std::make_unique<webrtc::SimulatedClock>(config.start_time_us);
      clock = owned_clock.get();
      auto trials = config.queue_pushback ?
                      "WebRTC-AddPacingToCongestionWindowPushback/Enabled/WebRTC-CongestionWindow/QueueSize:" +
                        std::to_string(config.queue_delay_ms) + ",MinBitrate:30000,DropFrame:true/" :
                      std::string {};
      if (config.loss_recovery_without_padding)
        trials += "WebRTC-Bwe-LossBasedBweV2/Enabled,PaddingDuration:0ms/";
      auto probe_log = std::make_unique<probe_event_log_t>(state, *clock, config.first_probe_cluster_id);
      return webrtc::CreateEnvironment(std::move(owned_clock), std::move(probe_log), webrtc::FieldTrials::Create(trials));
    }

    webrtc::NetworkControllerConfig
    make_configuration(const googcc_config_t &config,
      const webrtc::Environment &environment) {
      webrtc::NetworkControllerConfig upstream(environment);
      upstream.constraints.at_time = webrtc::Timestamp::Micros(config.start_time_us);
      upstream.constraints.min_data_rate = webrtc::DataRate::KilobitsPerSec(config.minimum_kbps);
      upstream.constraints.max_data_rate = webrtc::DataRate::KilobitsPerSec(config.maximum_kbps);
      upstream.constraints.starting_rate = webrtc::DataRate::KilobitsPerSec(config.initial_kbps);
      upstream.stream_based_config.requests_alr_probing = config.periodic_alr_probing;
      return upstream;
    }

    webrtc::SentPacket
    map_sent(const sent_packet_t &packet, std::int64_t cc_sequence, std::uint64_t data_in_flight,
      std::int32_t probe_base, std::int64_t probe_floor, std::int64_t probe_highest) {
      webrtc::SentPacket upstream;
      upstream.sequence_number = cc_sequence;
      upstream.send_time = webrtc::Timestamp::Micros(packet.send_time_us);
      upstream.size = webrtc::DataSize::Bytes(packet.ip_bytes);
      upstream.data_in_flight = webrtc::DataSize::Bytes(data_in_flight);
      if (packet.probe.cluster_id >= probe_floor && packet.probe.cluster_id <= probe_highest) {
        upstream.pacing_info = webrtc::PacedPacketInfo(packet.probe.cluster_id - probe_base,
          packet.probe.min_packets, packet.probe.min_bytes);
        upstream.pacing_info.send_bitrate = webrtc::DataRate::KilobitsPerSec(packet.probe.send_kbps);
      }
      return upstream;
    }
  }  // namespace

  struct googcc_adapter_t::impl_t {
    explicit impl_t(const googcc_config_t &configuration):
        config(configuration),
        environment(make_environment(state, clock, config)),
        controller(make_configuration(config, environment), webrtc::GoogCcConfig {}),
        now_us(config.start_time_us), next_cc_sequence(config.first_cc_sequence),
        minimum_current_probe_cluster(config.first_probe_cluster_id) {
      state.last_generated_probe_cluster = static_cast<std::int64_t>(config.first_probe_cluster_id) - 1;
      state.loss_recovery_without_padding = config.loss_recovery_without_padding;
      consume(controller.OnNetworkAvailability({ webrtc::Timestamp::Micros(now_us), true }));
      consume(controller.OnProcessInterval({ webrtc::Timestamp::Micros(now_us), std::nullopt }));
    }

    bool
    valid_time(std::int64_t time_us) const {
      return time_us >= now_us && time_us <= max_time_us;
    }

    void
    advance(std::int64_t time_us) {
      clock->AdvanceTimeMicroseconds(time_us - now_us);
      now_us = time_us;
    }

    void
    consume(const webrtc::NetworkControlUpdate &update) {
      if (update.target_rate) {
        const auto ratio = update.target_rate->cwnd_reduce_ratio;
        if (!std::isfinite(ratio) || ratio < 0 || ratio > 1) throw std::runtime_error("Invalid upstream encoder reduction");
        state.encoder_reduce_ratio = ratio;
        const auto loss = update.target_rate->network_estimate.loss_rate_ratio;
        if (!std::isfinite(loss) || loss < 0 || loss > 1) throw std::runtime_error("Invalid upstream network loss");
        state.network_loss_ppm = static_cast<std::uint32_t>(std::llround(loss * 1000000.0));
        const auto rtt = update.target_rate->network_estimate.round_trip_time;
        state.network_rtt_us = rtt.IsFinite() ? rtt.us() : -1;
        state.target_kbps = update.target_rate->target_rate.kbps();
        state.application_limited = !update.target_rate->is_bandwidth_limited;
        state.target_updated_at_us = update.target_rate->at_time.us();
      }
      if (update.pacer_config) {
        state.pacing_kbps = update.pacer_config->data_rate().kbps();
        state.requested_padding_kbps = update.pacer_config->pad_rate().kbps();
        state.padding_updated_at_us = update.pacer_config->at_time.us();
      }
      if (update.congestion_window && update.congestion_window->IsFinite()) {
        state.congestion_window_bytes = update.congestion_window->bytes();
      }
      for (const auto &probe : update.probe_cluster_configs) {
        const auto physical_id = static_cast<std::int64_t>(probe.id) + config.first_probe_cluster_id;
        if (probe.id < 0 || physical_id > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Probe identity exhausted");
        state.last_generated_probe_cluster = std::max(state.last_generated_probe_cluster, physical_id);
        if (probes.size() == 32) {
          ++state.discarded_probe_requests;
          continue;
        }
        probes.push_back({ static_cast<std::int32_t>(physical_id), probe.target_data_rate.kbps(), probe.target_duration.us(),
          probe.min_probe_delta.us(), probe.target_probe_count, probe.at_time.us() });
      }
    }

    const googcc_config_t config;
    // Only wire-to-controller identity translation. No receive/missing states,
    // packet matching, byte accounting, or report replay window lives here.
    std::unordered_map<std::uint64_t, std::int64_t> sequence_mapping;
    std::deque<std::uint64_t> sequence_order;
    googcc_snapshot_t state;
    webrtc::SimulatedClock *clock = nullptr;
    webrtc::Environment environment;
    webrtc::GoogCcNetworkController controller;
    std::int64_t now_us;
    std::int64_t next_cc_sequence;
    std::uint64_t last_event_sequence = 0;
    std::uint64_t receiver_epoch = 0;
    std::int64_t receiver_anchor_us = 0;
    std::int64_t sender_anchor_us = 0;
    std::int64_t minimum_current_probe_cluster = 0;
    std::vector<googcc_probe_t> probes;
  };

  googcc_adapter_t::googcc_adapter_t(const googcc_config_t &config) {
    if (config.connection_epoch == 0 || config.start_time_us < 0 || config.start_time_us > max_time_us ||
        config.minimum_kbps <= 0 || config.maximum_kbps < config.minimum_kbps ||
        config.initial_kbps < config.minimum_kbps || config.initial_kbps > config.maximum_kbps ||
        config.history_capacity == 0 || config.history_capacity > send_ledger_t::max_capacity ||
        config.first_cc_sequence < 0 || config.first_cc_sequence == std::numeric_limits<std::int64_t>::max() ||
        config.first_probe_cluster_id < 0 || config.first_probe_cluster_id > std::numeric_limits<std::int32_t>::max() - 32 ||
        (config.queue_pushback && !config.pacer_queue_feedback) || config.queue_delay_ms < 5 || config.queue_delay_ms > 1000) {
      throw std::invalid_argument("Invalid GoogCC constraints");
    }
    impl_ = std::make_unique<impl_t>(config);
  }

  googcc_adapter_t::~googcc_adapter_t() = default;

  bool
  googcc_adapter_t::on_successful_send(const successful_send_event_t &event) {
    const auto &packet = event.sent;
    if (event.connection_epoch != impl_->config.connection_epoch || event.event_sequence <= impl_->last_event_sequence ||
        !impl_->valid_time(packet.send_time_us) ||
        event.data_in_flight_bytes > static_cast<std::uint64_t>(max_time_us) ||
        impl_->next_cc_sequence == std::numeric_limits<std::int64_t>::max() ||
        impl_->sequence_mapping.contains(packet.extended_sequence)) {
      ++impl_->state.rejected_send_events;
      return false;
    }
    const auto cc_sequence = impl_->next_cc_sequence;
    // Finish bounded allocations before advancing either the owner or GoogCC.
    impl_->sequence_mapping.emplace(packet.extended_sequence, cc_sequence);
    try {
      impl_->sequence_order.push_back(packet.extended_sequence);
    }
    catch (...) {
      impl_->sequence_mapping.erase(packet.extended_sequence);
      throw;
    }
    if (impl_->sequence_order.size() > impl_->config.history_capacity) {
      impl_->sequence_mapping.erase(impl_->sequence_order.front());
      impl_->sequence_order.pop_front();
      ++impl_->state.sequence_mapping_evictions;
    }
    ++impl_->next_cc_sequence;
    impl_->last_event_sequence = event.event_sequence;
    ++impl_->state.accepted_sends;
    impl_->state.sequence_mapping_size = impl_->sequence_mapping.size();
    impl_->advance(packet.send_time_us);
    impl_->consume(impl_->controller.OnSentPacket(map_sent(packet, cc_sequence, event.data_in_flight_bytes,
      impl_->config.first_probe_cluster_id, impl_->minimum_current_probe_cluster, impl_->state.last_generated_probe_cluster)));
    return true;
  }

  bool
  googcc_adapter_t::on_feedback(const feedback_event_t &event) {
    const auto reject = [&] { ++impl_->state.rejected_feedback_events; return false; };
    if (event.feedback.result != report_result_e::accepted || event.connection_epoch != impl_->config.connection_epoch ||
        event.event_sequence <= impl_->last_event_sequence || !impl_->valid_time(event.processing_time_us) ||
        event.receiver_clock_epoch == 0 || event.receiver_sample_time_us < 0 || event.receiver_sample_time_us > max_time_us ||
        event.data_in_flight_bytes > static_cast<std::uint64_t>(max_time_us) ||
        event.feedback.changes.size() > send_ledger_t::max_report_packets) {
      return reject();
    }
    const bool new_clock = event.receiver_clock_epoch != impl_->receiver_epoch;
    const auto receiver_anchor = new_clock ? event.receiver_sample_time_us : impl_->receiver_anchor_us;
    const auto sender_anchor = new_clock ? event.processing_time_us : impl_->sender_anchor_us;
    for (const auto &change : event.feedback.changes) {
      if (change.sent.send_time_us < 0 || change.sent.send_time_us > event.processing_time_us ||
          (change.status != packet_status_e::received && change.status != packet_status_e::missing)) {
        return reject();
      }
      if (change.status == packet_status_e::received) {
        if (change.first_arrival_us < 0 || change.first_arrival_us > event.receiver_sample_time_us) {
          return reject();
        }
        const auto aligned = sender_anchor + (change.first_arrival_us - receiver_anchor);
        if (aligned < 0 || aligned > max_time_us) {
          return reject();
        }
      }
    }
    impl_->last_event_sequence = event.event_sequence;
    impl_->state.last_accepted_feedback_us = event.processing_time_us;
    impl_->advance(event.processing_time_us);
    if (new_clock) {
      impl_->receiver_epoch = event.receiver_clock_epoch;
      impl_->receiver_anchor_us = receiver_anchor;
      impl_->sender_anchor_us = sender_anchor;
    }
    if (event.feedback.receiver_clock_changed) {
      impl_->state.last_covered_feedback_us = -1;
      impl_->state.last_covered_send_us = -1;
      impl_->minimum_current_probe_cluster = impl_->state.last_generated_probe_cluster + 1;
      impl_->state.last_native_probe_cluster = -1;
      impl_->state.last_native_probe_bps = 0;
      impl_->state.last_native_probe_at_us = -1;
      impl_->state.last_native_probe_failure = -1;
      webrtc::NetworkRouteChange reset;
      reset.at_time = webrtc::Timestamp::Micros(event.processing_time_us);
      reset.constraints.at_time = reset.at_time;
      reset.constraints.min_data_rate = webrtc::DataRate::KilobitsPerSec(impl_->config.minimum_kbps);
      reset.constraints.max_data_rate = webrtc::DataRate::KilobitsPerSec(impl_->config.maximum_kbps);
      reset.constraints.starting_rate = webrtc::DataRate::KilobitsPerSec(impl_->state.target_kbps);
      // Clock-domain changes invalidate requests made before the route reset.
      impl_->state.discarded_probe_requests += impl_->probes.size();
      impl_->probes.clear();
      impl_->consume(impl_->controller.OnNetworkRouteChange(reset));
      ++impl_->state.receiver_clock_resets;
    }
    if (!event.feedback.changes.empty()) {
      webrtc::TransportPacketsFeedback upstream;
      upstream.feedback_time = webrtc::Timestamp::Micros(event.processing_time_us);
      upstream.data_in_flight = webrtc::DataSize::Bytes(event.data_in_flight_bytes);
      upstream.packet_feedbacks.reserve(event.feedback.changes.size());
      auto latest_covered_send_us = impl_->state.last_covered_send_us;
      for (const auto &change : event.feedback.changes) {
        const auto mapping = impl_->sequence_mapping.find(change.sent.extended_sequence);
        if (mapping == impl_->sequence_mapping.end()) {
          ++impl_->state.unmapped_feedback_changes;
          continue;  // Expired/unsubmitted controller identity is unknown, not loss.
        }
        latest_covered_send_us = std::max(latest_covered_send_us, change.sent.send_time_us);
        webrtc::PacketResult packet;
        packet.sent_packet = map_sent(change.sent, mapping->second, 0,
          impl_->config.first_probe_cluster_id, impl_->minimum_current_probe_cluster, impl_->state.last_generated_probe_cluster);
        if (change.sent.probe.cluster_id >= 0 && change.sent.probe.cluster_id < impl_->minimum_current_probe_cluster) {
          // A delayed receipt still contributes ordinary delivery/loss bytes,
          // but a cluster made before the clock/route reset cannot estimate
          // the new route. Keep the authoritative raw send record unchanged.
          packet.sent_packet.pacing_info = webrtc::PacedPacketInfo {};
          ++impl_->state.stale_probe_feedback_suppressed;
        }
        packet.reported_lost_for_the_first_time = change.first_missing;
        packet.reported_recovered_for_the_first_time = change.first_recovered;
        if (change.status == packet_status_e::received) {
          // One fixed offset per receiver epoch preserves all arrival deltas.
          // This is a clock-domain translation, not a one-way-delay measurement.
          packet.receive_time = webrtc::Timestamp::Micros(impl_->sender_anchor_us +
                                                          (change.first_arrival_us - impl_->receiver_anchor_us));
          packet.arrival_time_offset = webrtc::TimeDelta::Micros(event.receiver_sample_time_us - change.first_arrival_us);
        }
        upstream.packet_feedbacks.push_back(packet);
      }
      if (!upstream.packet_feedbacks.empty()) {
        impl_->state.last_covered_feedback_us = event.processing_time_us;
        impl_->state.last_covered_send_us = latest_covered_send_us;
        impl_->state.feedback_packet_changes += upstream.packet_feedbacks.size();
        impl_->consume(impl_->controller.OnTransportPacketsFeedback(std::move(upstream)));
        ++impl_->state.feedback_batches;
      }
    }
    return true;
  }

  bool
  googcc_adapter_t::process_interval(std::int64_t now_us, std::optional<googcc_queue_sample_t> queue) {
    if (!impl_->valid_time(now_us)) {
      return false;
    }
    if ((impl_->config.pacer_queue_feedback && !queue) ||
        (queue && (queue->connection_epoch != impl_->config.connection_epoch || queue->sampled_at_us != now_us ||
                    !queue->accounting_valid || queue->stopped || queue->queued_ip_bytes > static_cast<std::uint64_t>(max_time_us)))) {
      ++impl_->state.rejected_queue_samples;
      return false;
    }
    impl_->advance(now_us);
    std::optional<webrtc::DataSize> upstream_queue;
    if (queue) {
      upstream_queue = webrtc::DataSize::Bytes(queue->queued_ip_bytes);
      impl_->state.pacer_queue_ip_bytes = queue->queued_ip_bytes;
      impl_->state.pacer_queue_at_us = now_us;
      ++impl_->state.accepted_queue_samples;
    }
    impl_->consume(impl_->controller.OnProcessInterval({ webrtc::Timestamp::Micros(now_us), upstream_queue }));
    return true;
  }

  std::vector<googcc_probe_t>
  googcc_adapter_t::take_probe_requests() {
    return std::exchange(impl_->probes, {});
  }

  const googcc_snapshot_t &
  googcc_adapter_t::snapshot() const noexcept {
    return impl_->state;
  }

}  // namespace transport
