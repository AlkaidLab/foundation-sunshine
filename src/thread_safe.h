/**
 * @file src/thread_safe.h
 * @brief Declarations for thread-safe data structures.
 */
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "utility.h"

namespace safe {
  template <class T>
  class event_t {
  public:
    using status_t = util::optional_t<T>;

    template <class... Args>
    void
    raise(Args &&...args) {
      std::lock_guard lg { _lock };
      if (!_continue.load(std::memory_order_acquire)) {
        return;
      }

      if constexpr (std::is_same_v<std::optional<T>, status_t>) {
        _status = std::make_optional<T>(std::forward<Args>(args)...);
      }
      else {
        _status = status_t { std::forward<Args>(args)... };
      }

      _has_status.store(static_cast<bool>(_status), std::memory_order_release);
      _cv.notify_all();
    }

    // pop and view should not be used interchangeably
    status_t
    pop() {
      std::unique_lock ul { _lock };

      if (!_continue.load(std::memory_order_acquire)) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue.load(std::memory_order_acquire)) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      _has_status.store(false, std::memory_order_release);
      return val;
    }

    // pop and view should not be used interchangeably
    template <class Rep, class Period>
    status_t
    pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul { _lock };

      if (!_continue.load(std::memory_order_acquire)) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (!_continue.load(std::memory_order_acquire) || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      _has_status.store(false, std::memory_order_release);
      return val;
    }

    // pop and view should not be used interchangeably
    status_t
    view() {
      std::unique_lock ul { _lock };

      if (!_continue.load(std::memory_order_acquire)) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue.load(std::memory_order_acquire)) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    // pop and view should not be used interchangeably
    template <class Rep, class Period>
    status_t
    view(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul { _lock };

      if (!_continue.load(std::memory_order_acquire)) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (!_continue.load(std::memory_order_acquire) || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    bool
    peek() {
      return _continue.load(std::memory_order_acquire) && _has_status.load(std::memory_order_acquire);
    }

    void
    stop() {
      std::lock_guard lg { _lock };

      _continue.store(false, std::memory_order_release);

      _cv.notify_all();
    }

    void
    reset() {
      std::lock_guard lg { _lock };

      _continue.store(true, std::memory_order_release);

      _status = util::false_v<status_t>;
      _has_status.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool
    running() const {
      return _continue.load(std::memory_order_acquire);
    }

  private:
    std::atomic_bool _continue { true };
    std::atomic_bool _has_status { false };
    status_t _status { util::false_v<status_t> };

    std::condition_variable _cv;
    std::mutex _lock;
  };

  template <class T>
  class alarm_raw_t {
  public:
    using status_t = util::optional_t<T>;

    void
    ring(const status_t &status) {
      std::lock_guard lg(_lock);

      _status = status;
      _rang = true;
      _cv.notify_one();
    }

    void
    ring(status_t &&status) {
      std::lock_guard lg(_lock);

      _status = std::move(status);
      _rang = true;
      _cv.notify_one();
    }

    template <class Rep, class Period>
    auto
    wait_for(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this]() { return _rang; });
    }

    template <class Rep, class Period, class Pred>
    auto
    wait_for(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this, &pred]() { return _rang || pred(); });
    }

    template <class Rep, class Period>
    auto
    wait_until(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this]() { return _rang; });
    }

    template <class Rep, class Period, class Pred>
    auto
    wait_until(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this, &pred]() { return _rang || pred(); });
    }

    auto
    wait() {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this]() { return _rang; });
    }

    template <class Pred>
    auto
    wait(Pred &&pred) {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this, &pred]() { return _rang || pred(); });
    }

    const status_t &
    status() const {
      return _status;
    }

    status_t &
    status() {
      return _status;
    }

    void
    reset() {
      _status = status_t {};
      _rang = false;
    }

  private:
    std::mutex _lock;
    std::condition_variable _cv;

    status_t _status { util::false_v<status_t> };
    bool _rang { false };
  };

  template <class T>
  using alarm_t = std::shared_ptr<alarm_raw_t<T>>;

  template <class T>
  alarm_t<T>
  make_alarm() {
    return std::make_shared<alarm_raw_t<T>>();
  }

  template <class T>
  class queue_t {
  public:
    using status_t = util::optional_t<T>;

    enum class replace_result_e { inserted, replaced, full, stopped };

    queue_t(std::uint32_t max_elements = 32):
        _max_elements { max_elements } {}

    // Replace a matching waiting item in place, preserving other producers'
    // order. When no item matches, never clear the queue on overflow.
    // The caller's predicate is evaluated while the queue lock is held.
    template <class Predicate>
    replace_result_e
    raise_or_replace(T value, Predicate &&matches) {
      std::unique_lock ul { _lock };
      if (!_continue) return replace_result_e::stopped;
      for (auto &item : _queue) {
        if (matches(item)) {
          [[maybe_unused]] auto old = std::move(item);
          item = std::move(value);
          ul.unlock();
          _cv.notify_all();
          return replace_result_e::replaced;
        }
      }
      if (_queue.size() >= _max_elements) return replace_result_e::full;
      _queue.emplace_back(std::move(value));
      ul.unlock();
      _cv.notify_all();
      return replace_result_e::inserted;
    }

    // Loss-intolerant producers must not invoke raise()'s legacy clear-on-full.
    // Failure is explicit; existing producers retain their original behavior.
    template <class... Args>
    bool
    try_raise(Args &&...args) {
      std::lock_guard ul { _lock };
      if (!_continue || _queue.size() >= _max_elements) return false;
      _queue.emplace_back(std::forward<Args>(args)...);
      _cv.notify_all();
      return true;
    }

    template <class... Args>
    void
    raise(Args &&...args) {
      std::lock_guard ul { _lock };

      if (!_continue) {
        return;
      }

      if (_queue.size() == _max_elements) {
        // raise() on a full queue drops everything that waits; telemetry counts these (stream health: XP_QUEUE_FLUSH)
        _clears.fetch_add(1, std::memory_order_relaxed);
        _cleared_elements.fetch_add(_queue.size(), std::memory_order_relaxed);
        _queue.clear();
      }

      _queue.emplace_back(std::forward<Args>(args)...);

      _cv.notify_all();
    }

    /// How many times raise() found the queue full and cleared it, and how many elements those clears dropped.
    std::pair<std::uint64_t, std::uint64_t>
    clear_stats() {
      // both counters are written under the lock in raise(); read under it too, so a clear is seen with its own element count
      std::lock_guard ul { _lock };
      return { _clears.load(std::memory_order_relaxed), _cleared_elements.load(std::memory_order_relaxed) };
    }

    bool
    peek() {
      // raise() and try_raise() change _queue under _lock; reading it unlocked is a data race.
      std::lock_guard ul { _lock };
      return _continue && !_queue.empty();
    }

    template <class Rep, class Period>
    status_t
    pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul { _lock };

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    status_t
    pop() {
      std::unique_lock ul { _lock };

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    std::vector<T> &
    unsafe() {
      return _queue;
    }

    void
    stop() {
      std::lock_guard lg { _lock };

      _continue = false;

      _cv.notify_all();
    }

    [[nodiscard]] bool
    running() const {
      return _continue;
    }

  private:
    bool _continue { true };
    std::uint32_t _max_elements;
    std::atomic<std::uint64_t> _clears { 0 };
    std::atomic<std::uint64_t> _cleared_elements { 0 };

    std::mutex _lock;
    std::condition_variable _cv;

    std::vector<T> _queue;
  };

  template <class T>
  class shared_t {
  public:
    using element_type = T;

    using construct_f = std::function<int(element_type &)>;
    using destruct_f = std::function<void(element_type &)>;

    struct ptr_t {
      shared_t *owner;

      ptr_t():
          owner { nullptr } {}
      explicit ptr_t(shared_t *owner):
          owner { owner } {}

      ptr_t(ptr_t &&ptr) noexcept:
          owner { ptr.owner } {
        ptr.owner = nullptr;
      }

      ptr_t(const ptr_t &ptr) noexcept:
          owner { ptr.owner } {
        if (!owner) {
          return;
        }

        auto tmp = ptr.owner->ref();
        tmp.owner = nullptr;
      }

      ptr_t &
      operator=(const ptr_t &ptr) noexcept {
        if (!ptr.owner) {
          release();

          return *this;
        }

        return *this = std::move(*ptr.owner->ref());
      }

      ptr_t &
      operator=(ptr_t &&ptr) noexcept {
        if (owner) {
          release();
        }

        std::swap(owner, ptr.owner);

        return *this;
      }

      ~ptr_t() {
        if (owner) {
          release();
        }
      }

      operator bool() const {
        return owner != nullptr;
      }

      void
      release() {
        std::lock_guard lg { owner->_lock };

        if (!--owner->_count) {
          owner->_destruct(*get());
          (*this)->~element_type();
        }

        owner = nullptr;
      }

      element_type *
      get() const {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }

      element_type *
      operator->() {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }
    };

    template <class FC, class FD>
    shared_t(FC &&fc, FD &&fd):
        _construct { std::forward<FC>(fc) }, _destruct { std::forward<FD>(fd) } {}
    [[nodiscard]] ptr_t
    ref() {
      std::lock_guard lg { _lock };

      if (!_count) {
        new (_object_buf.data()) element_type;
        if (_construct(*reinterpret_cast<element_type *>(_object_buf.data()))) {
          return ptr_t { nullptr };
        }
      }

      ++_count;

      return ptr_t { this };
    }

    /**
     * @brief Check if there are any active references without creating a new one.
     * @return true if there are active references, false otherwise.
     */
    [[nodiscard]] bool
    has_ref() {
      std::lock_guard lg { _lock };
      return _count > 0;
    }

    /**
     * @brief Atomically acquire a reference only if one already exists.
     *        Unlike ref(), this never invokes the construct callback, which makes
     *        it safe to use from code paths that must not (re-)start the underlying
     *        resource (e.g. probing whether a long-running context is still alive).
     * @return A live ptr_t when there is an existing reference, an empty one otherwise.
     */
    [[nodiscard]] ptr_t
    try_ref() {
      std::lock_guard lg { _lock };
      if (!_count) {
        return ptr_t { nullptr };
      }
      ++_count;
      return ptr_t { this };
    }

  private:
    construct_f _construct;
    destruct_f _destruct;

    std::array<std::uint8_t, sizeof(element_type)> _object_buf;

    std::uint32_t _count;
    std::mutex _lock;
  };

  template <class T, class F_Construct, class F_Destruct>
  auto
  make_shared(F_Construct &&fc, F_Destruct &&fd) {
    return shared_t<T> {
      std::forward<F_Construct>(fc), std::forward<F_Destruct>(fd)
    };
  }

  using signal_t = event_t<bool>;

  class mail_raw_t;
  using mail_t = std::shared_ptr<mail_raw_t>;

  void
  cleanup(mail_raw_t *);
  template <class T>
  class post_t: public T {
  public:
    template <class... Args>
    post_t(mail_t mail, Args &&...args):
        T(std::forward<Args>(args)...), mail { std::move(mail) } {}

    mail_t mail;

    ~post_t() {
      cleanup(mail.get());
    }
  };

  template <class T>
  inline auto
  lock(const std::weak_ptr<void> &wp) {
    return std::reinterpret_pointer_cast<typename T::element_type>(wp.lock());
  }

  class mail_raw_t: public std::enable_shared_from_this<mail_raw_t> {
  public:
    template <class T>
    using event_t = std::shared_ptr<post_t<event_t<T>>>;

    template <class T>
    using queue_t = std::shared_ptr<post_t<queue_t<T>>>;

    template <class T>
    event_t<T>
    event(const std::string_view &id) {
      std::lock_guard lg { mutex };

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        return lock<event_t<T>>(it->second);
      }

      auto post = std::make_shared<typename event_t<T>::element_type>(shared_from_this());
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> { std::string { id }, post });

      return post;
    }

    template <class T>
    queue_t<T>
    queue(const std::string_view &id) {
      std::lock_guard lg { mutex };

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        return lock<queue_t<T>>(it->second);
      }

      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), 32);
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> { std::string { id }, post });

      return post;
    }

    void
    cleanup() {
      std::lock_guard lg { mutex };

      for (auto it = std::begin(id_to_post); it != std::end(id_to_post); ++it) {
        auto &weak = it->second;

        if (weak.expired()) {
          id_to_post.erase(it);

          return;
        }
      }
    }

    std::mutex mutex;

    std::map<std::string, std::weak_ptr<void>, std::less<>> id_to_post;
  };

  inline void
  cleanup(mail_raw_t *mail) {
    mail->cleanup();
  }
}  // namespace safe
