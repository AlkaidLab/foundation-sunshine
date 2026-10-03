#include "transport_owner_inbox.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>

namespace transport {
  namespace {
    void saturating_add(std::uint64_t &target, std::uint64_t value) {
      target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
    }
  }

  owner_flow_t::owner_flow_t(std::uint64_t id, std::uint64_t epoch, std::shared_ptr<void> value, std::shared_ptr<const void> cookie) :
      handle(id), connection_epoch(epoch), context(std::move(value)), cookie_(std::move(cookie)) {}
  bool owner_flow_t::is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
  bool owner_flow_t::is_drained() const noexcept { return drained_.load(std::memory_order_acquire); }
  void owner_flow_t::wait_drained() const {
    std::unique_lock lock(drain_mutex_);
    drain_cv_.wait(lock, [&] { return is_drained(); });
  }
  bool owner_flow_t::wait_drained_until(std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(drain_mutex_);
    return drain_cv_.wait_until(lock, deadline, [&] { return is_drained(); });
  }
  void owner_flow_t::mark_drained() const {
    {
      std::lock_guard lock(drain_mutex_);
      drained_.store(true, std::memory_order_release);
    }
    drain_cv_.notify_all();
  }
  bool owner_drop_counts_t::empty() const noexcept {
    return !full_frames && !closed_frames && !invalid_frames && !stale_frames && !rejected_bytes &&
           !discarded_frames && !discarded_bytes && !dropped_feedback && !dropped_feedback_bytes;
  }

  class owner_inbox_t::impl_t {
  public:
    struct flow_state_t {
      owner_flow_ref_t flow;
      pacing_limits_t initial_limits;
      std::deque<owner_frame_t> frames;
      std::deque<owner_feedback_t> feedback;
      bool registration_pending = true;
      bool close_pending = false;
      bool close_delivered = false;
      std::optional<pacing_limits_t> latest_limits;
      std::optional<std::uint64_t> highest_break;
      std::optional<std::uint64_t> last_break;
      owner_drop_counts_t drops;
    };

    explicit impl_t(owner_inbox_bounds_t value) : bounds(value), cookie(std::make_shared<const int>(0)) {
      if (!bounds.maximum_flows || bounds.maximum_flows > 256 ||
          !bounds.maximum_queued_frames || bounds.maximum_queued_frames > 65536 ||
          !bounds.maximum_queued_bytes || bounds.maximum_queued_bytes > 128U * 1024U * 1024U ||
          !bounds.maximum_frames_per_flow || bounds.maximum_frames_per_flow > 256) {
        throw std::invalid_argument("invalid owner inbox bounds");
      }
      slots.resize(bounds.maximum_flows);
    }

    bool owns(const owner_flow_ref_t &flow) const { return flow && flow->cookie_ == cookie; }
    flow_state_t *find(const owner_flow_ref_t &flow) {
      if (!owns(flow)) return nullptr;
      for (auto &slot : slots) if (slot && slot->flow == flow) return slot.get();
      return nullptr;
    }
    bool pending(const flow_state_t &state) const {
      return state.close_pending || (state.flow->is_closed() && !state.close_delivered) || state.registration_pending ||
             state.latest_limits.has_value() || state.highest_break.has_value() || !state.drops.empty();
    }
    void notify() {
      ++generation;
      cv.notify_all();
    }
    void break_reference(flow_state_t &state, std::uint64_t frame_id) {
      state.last_break = state.last_break ? std::max(*state.last_break, frame_id) : frame_id;
      state.highest_break = state.last_break;
    }
    void reject(flow_state_t *state, owner_submit_result_e reason, std::size_t bytes) {
      auto count = [&](owner_drop_counts_t &drops) {
        if (reason == owner_submit_result_e::full) saturating_add(drops.full_frames, 1);
        else if (reason == owner_submit_result_e::closed) saturating_add(drops.closed_frames, 1);
        else if (reason == owner_submit_result_e::stale) saturating_add(drops.stale_frames, 1);
        else saturating_add(drops.invalid_frames, 1);
        saturating_add(drops.rejected_bytes, bytes);
      };
      count(totals);
      if (state) count(state->drops);
    }
    void reject_feedback(flow_state_t *state, std::size_t bytes) {
      saturating_add(totals.dropped_feedback, 1);
      saturating_add(totals.dropped_feedback_bytes, bytes);
      if (state) {
        saturating_add(state->drops.dropped_feedback, 1);
        saturating_add(state->drops.dropped_feedback_bytes, bytes);
      }
    }
    void remove_frame(flow_state_t &state, std::size_t index, owner_command_t &command) {
      auto &frame = state.frames[index];
      queued_bytes -= frame.owned_bytes;
      --queued_frames;
      saturating_add(command.drops.discarded_frames, 1);
      saturating_add(command.drops.discarded_bytes, frame.owned_bytes);
      saturating_add(totals.discarded_frames, 1);
      saturating_add(totals.discarded_bytes, frame.owned_bytes);
      command.discarded_frames.push_back(std::move(frame));
      state.frames.erase(state.frames.begin() + static_cast<std::ptrdiff_t>(index));
    }

    owner_inbox_bounds_t bounds;
    const std::shared_ptr<const void> cookie;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::unique_ptr<flow_state_t>> slots;
    std::size_t cursor = 0;
    std::size_t feedback_cursor = 0;
    std::size_t queued_frames = 0;
    std::size_t queued_bytes = 0;
    std::size_t feedback_messages = 0;
    std::size_t feedback_bytes = 0;
    owner_drop_counts_t totals;
    std::uint64_t next_handle = 1;
    std::uint64_t generation = 0;
    bool stopped = false;
  };

  owner_inbox_t::owner_inbox_t(owner_inbox_bounds_t bounds) : impl_(std::make_unique<impl_t>(bounds)) {}
  owner_inbox_t::~owner_inbox_t() { stop(); }

  owner_flow_ref_t owner_inbox_t::add_flow(std::uint64_t epoch, std::shared_ptr<void> context, const pacing_limits_t &limits) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    if (!epoch || p.stopped || p.next_handle == std::numeric_limits<std::uint64_t>::max()) return {};
    for (const auto &slot : p.slots) if (slot && slot->flow->connection_epoch == epoch) return {};
    const auto where = std::find_if(p.slots.begin(), p.slots.end(), [](const auto &slot) { return !slot; });
    if (where == p.slots.end()) return {};
    auto state = std::make_unique<impl_t::flow_state_t>();
    state->flow = owner_flow_ref_t(new owner_flow_t(p.next_handle, epoch, std::move(context), p.cookie));
    state->initial_limits = limits;
    const auto flow = state->flow;
    *where = std::move(state);
    ++p.next_handle;
    p.notify();
    return flow;
  }

  owner_submit_result_e owner_inbox_t::submit(owner_frame_t frame) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    auto *state = p.find(frame.flow);
    if (!p.owns(frame.flow)) {
      p.reject(nullptr, owner_submit_result_e::invalid, frame.owned_bytes);
      return owner_submit_result_e::unknown_flow;
    }
    if (frame.flow->is_closed() || p.stopped) {
      p.reject(state, owner_submit_result_e::closed, frame.owned_bytes);
      if (state) p.notify();
      return owner_submit_result_e::closed;
    }
    if (!state) return owner_submit_result_e::unknown_flow;
    if (!frame.payload || !frame.owned_bytes || (frame.policy && frame.policy->connection_epoch != frame.flow->connection_epoch)) {
      p.reject(state, owner_submit_result_e::invalid, frame.owned_bytes);
      p.notify();
      return owner_submit_result_e::invalid;
    }
    if (state->last_break && frame.frame_id <= *state->last_break) {
      p.reject(state, owner_submit_result_e::stale, frame.owned_bytes);
      p.notify();
      return owner_submit_result_e::stale;
    }
    const bool full = p.queued_frames >= p.bounds.maximum_queued_frames ||
                      state->frames.size() >= p.bounds.maximum_frames_per_flow ||
                      frame.owned_bytes > p.bounds.maximum_queued_bytes - p.queued_bytes;
    if (full) {
      p.reject(state, owner_submit_result_e::full, frame.owned_bytes);
      if (frame.dependency != frame_dependency_e::non_reference) p.break_reference(*state, frame.frame_id);
      p.notify();
      return owner_submit_result_e::full;
    }
    const auto bytes = frame.owned_bytes;
    const auto id = frame.frame_id;
    const auto dependency = frame.dependency;
    try { state->frames.push_back(std::move(frame)); }
    catch (const std::bad_alloc &) {
      p.reject(state, owner_submit_result_e::full, bytes);
      if (dependency != frame_dependency_e::non_reference) p.break_reference(*state, id);
      p.notify();
      return owner_submit_result_e::full;
    }
    ++p.queued_frames;
    p.queued_bytes += bytes;
    p.notify();
    return owner_submit_result_e::accepted;
  }

  owner_submit_result_e owner_inbox_t::submit_feedback(const owner_flow_ref_t &flow, std::vector<std::uint8_t> bytes, std::int64_t received_at_us) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    auto *state = p.find(flow);
    if (!p.owns(flow)) { p.reject_feedback(nullptr, bytes.size()); return owner_submit_result_e::unknown_flow; }
    if (flow->is_closed() || p.stopped) {
      p.reject_feedback(state, bytes.size());
      if (state) p.notify();
      return owner_submit_result_e::closed;
    }
    if (!state) return owner_submit_result_e::unknown_flow;
    if (bytes.empty() || bytes.size() > max_owner_feedback_bytes || bytes.capacity() > max_owner_feedback_bytes || received_at_us < 0) {
      p.reject_feedback(state, bytes.size());
      p.notify();
      return owner_submit_result_e::invalid;
    }
    if (state->feedback.size() >= max_owner_feedback_per_flow || p.feedback_messages >= max_owner_feedback_messages ||
        bytes.size() > max_owner_feedback_queued_bytes - p.feedback_bytes) {
      p.reject_feedback(state, bytes.size());
      p.notify();
      return owner_submit_result_e::full;
    }
    const auto size = bytes.size();
    try { state->feedback.push_back({flow, std::move(bytes), received_at_us}); }
    catch (const std::bad_alloc &) { p.reject_feedback(state, size); p.notify(); return owner_submit_result_e::full; }
    ++p.feedback_messages;
    p.feedback_bytes += size;
    p.notify();
    return owner_submit_result_e::accepted;
  }

  bool owner_inbox_t::update_limits(const owner_flow_ref_t &flow, const pacing_limits_t &limits) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    auto *state = p.find(flow);
    if (!state || flow->is_closed() || p.stopped) return false;
    state->latest_limits = limits;
    p.notify();
    return true;
  }

  bool owner_inbox_t::mark_reference_break(const owner_flow_ref_t &flow, std::uint64_t frame_id) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    auto *state = p.find(flow);
    if (!state || flow->is_closed() || p.stopped) return false;
    p.break_reference(*state, frame_id);
    p.notify();
    return true;
  }

  bool owner_inbox_t::close(const owner_flow_ref_t &flow) {
    auto &p = *impl_;
    if (!p.owns(flow)) return false;
    // Seal already-taken frames before waiting for the queue mutex. The owner
    // independently checks this same gate before every actual UDP syscall.
    flow->closed_.store(true, std::memory_order_release);
    std::lock_guard lock(p.mutex);
    auto *state = p.find(flow);
    if (!state) return flow->is_drained();
    if (!state->close_delivered) {
      state->close_pending = true;
      p.notify();
    }
    return true;
  }

  void owner_inbox_t::stop() {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    if (p.stopped) return;
    p.stopped = true;
    for (auto &slot : p.slots) if (slot) {
      slot->flow->closed_.store(true, std::memory_order_release);
      if (!slot->close_delivered) slot->close_pending = true;
    }
    p.notify();
  }

  std::vector<owner_command_t> owner_inbox_t::take_commands() {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    std::vector<owner_command_t> commands;
    commands.reserve(p.slots.size());
    // Build/reserve every command before moving queues or clearing command
    // slots. Allocation failure cannot lose an owned frame or pending close.
    for (int pass = 0; pass < 2; ++pass) for (const auto &slot : p.slots) {
      if (!slot || !p.pending(*slot)) continue;
      const bool closing = slot->close_pending || (slot->flow->is_closed() && !slot->close_delivered);
      if (closing != (pass == 0)) continue;
      owner_command_t command;
      command.flow = slot->flow;
      command.close = closing;
      command.register_flow = !command.close && slot->registration_pending;
      command.initial_limits = slot->initial_limits;
      if (!command.close) {
        command.latest_limits = slot->latest_limits;
        command.highest_reference_break = slot->highest_break;
      }
      command.drops = slot->drops;
      command.discarded_frames.reserve(slot->frames.size());
      commands.push_back(std::move(command));
    }
    for (auto &command : commands) {
      auto &state = *p.find(command.flow);
      // close seals the gate before acquiring this mutex, so it may have
      // arrived during the allocation phase. Upgrade before clearing slots.
      if (!command.close && state.flow->is_closed() && !state.close_delivered) {
        command.close = true;
        command.register_flow = false;
        command.latest_limits.reset();
        command.highest_reference_break.reset();
      }
      for (std::size_t i = 0; i < state.frames.size();) {
        if (command.close || (command.highest_reference_break && state.frames[i].frame_id <= *command.highest_reference_break)) p.remove_frame(state, i, command);
        else ++i;
      }
      state.registration_pending = false;
      state.latest_limits.reset();
      state.highest_break.reset();
      state.drops = {};
      if (command.close) {
        for (const auto &feedback : state.feedback) {
          --p.feedback_messages;
          p.feedback_bytes -= feedback.authenticated_bytes.size();
          saturating_add(command.drops.dropped_feedback, 1);
          saturating_add(command.drops.dropped_feedback_bytes, feedback.authenticated_bytes.size());
          saturating_add(p.totals.dropped_feedback, 1);
          saturating_add(p.totals.dropped_feedback_bytes, feedback.authenticated_bytes.size());
        }
        state.feedback.clear();
        state.close_pending = false;
        state.close_delivered = true;
      }
    }
    std::partition(commands.begin(), commands.end(), [](const auto &command) { return command.close; });
    return commands;
  }

  std::optional<owner_frame_t> owner_inbox_t::take_frame() {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    for (std::size_t visited = 0; visited < p.slots.size(); ++visited) {
      const auto index = (p.cursor + visited) % p.slots.size();
      auto &slot = p.slots[index];
      if (!slot || slot->flow->is_closed() || slot->registration_pending || slot->highest_break || slot->frames.empty()) continue;
      auto frame = std::move(slot->frames.front());
      slot->frames.pop_front();
      --p.queued_frames;
      p.queued_bytes -= frame.owned_bytes;
      p.cursor = (index + 1) % p.slots.size();
      return frame;
    }
    return {};
  }

  std::optional<owner_feedback_t> owner_inbox_t::take_feedback() {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    for (std::size_t visited = 0; visited < p.slots.size(); ++visited) {
      const auto index = (p.feedback_cursor + visited) % p.slots.size();
      auto &slot = p.slots[index];
      if (!slot || slot->flow->is_closed() || slot->registration_pending || slot->feedback.empty()) continue;
      auto feedback = std::move(slot->feedback.front());
      slot->feedback.pop_front();
      --p.feedback_messages;
      p.feedback_bytes -= feedback.authenticated_bytes.size();
      p.feedback_cursor = (index + 1) % p.slots.size();
      return feedback;
    }
    return {};
  }

  bool owner_inbox_t::acknowledge_drained(const owner_flow_ref_t &flow) {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    if (!p.owns(flow)) return false;
    auto *state = p.find(flow);
    if (!state) return flow->is_drained();
    if (!flow->is_closed() || !state->close_delivered || !state->frames.empty() || !state->feedback.empty()) return false;
    const auto where = std::find_if(p.slots.begin(), p.slots.end(), [&](const auto &slot) { return slot.get() == state; });
    where->reset();
    flow->mark_drained();
    p.notify();
    return true;
  }

  bool owner_inbox_t::emergency_drained_after_owner_abort() noexcept {
    auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    if (!p.stopped) return false;
    for (auto &slot : p.slots) {
      if (!slot) continue;
      for (const auto &frame : slot->frames) {
        saturating_add(p.totals.discarded_frames, 1);
        saturating_add(p.totals.discarded_bytes, frame.owned_bytes);
      }
      for (const auto &feedback : slot->feedback) {
        saturating_add(p.totals.dropped_feedback, 1);
        saturating_add(p.totals.dropped_feedback_bytes, feedback.authenticated_bytes.size());
      }
      auto flow = slot->flow;
      // No encoder payload, pending feedback, or queue slot survives this
      // barrier. The decoupled flow reference itself never owns a session.
      slot.reset();
      flow->mark_drained();
    }
    p.queued_frames = p.queued_bytes = p.feedback_messages = p.feedback_bytes = 0;
    p.notify();
    return true;
  }

  std::uint64_t owner_inbox_t::wake_generation() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->generation;
  }
  owner_wait_result_e owner_inbox_t::wait_until(std::uint64_t generation, std::chrono::steady_clock::time_point deadline) {
    auto &p = *impl_;
    std::unique_lock lock(p.mutex);
    p.cv.wait_until(lock, deadline, [&] { return p.stopped || p.generation != generation; });
    if (p.stopped) return owner_wait_result_e::stopped;
    return p.generation != generation ? owner_wait_result_e::woken : owner_wait_result_e::deadline;
  }
  owner_inbox_snapshot_t owner_inbox_t::snapshot() const {
    const auto &p = *impl_;
    std::lock_guard lock(p.mutex);
    owner_inbox_snapshot_t result;
    result.queued_frames = p.queued_frames;
    result.queued_bytes = p.queued_bytes;
    result.queued_feedback_messages = p.feedback_messages;
    result.queued_feedback_bytes = p.feedback_bytes;
    result.drops = p.totals;
    result.stopped = p.stopped;
    for (const auto &slot : p.slots) if (slot) { ++result.flows; result.pending_commands += p.pending(*slot); }
    return result;
  }
}  // namespace transport
