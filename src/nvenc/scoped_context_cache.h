/**
 * @file src/nvenc/scoped_context_cache.h
 * @brief Weak context cache with explicit retention during encoder probing.
 */
#pragma once

#include <map>
#include <memory>
#include <mutex>

namespace nvenc {

  /**
   * @brief Share contexts between active encoders without keeping an idle GPU alive.
   *        A retention token additionally keeps contexts acquired during a probe
   *        alive between its short-lived encoder candidates. Overlapping probes
   *        share this retention until the last token is released.
   */
  template <typename Key, typename Context>
  class scoped_context_cache {
  public:
    /**
     * @brief Retain contexts acquired while the returned token is alive.
     *        Does not create a context or initialize the GPU.
     */
    std::shared_ptr<void>
    retain() {
      std::lock_guard lock(mutex);
      auto retained = retention.lock();
      if (!retained) {
        retained = std::make_shared<context_map>();
        retention = retained;
      }
      return retained;
    }

    /**
     * @brief Reuse a live context or create one, serializing concurrent requests.
     * @param create Returns a context, or nullptr on failure. Must not reenter this cache.
     */
    template <typename Create>
    std::shared_ptr<Context>
    acquire(const Key &key, Create create) {
      // Keep the retention holder alive until after unlocking, even if the last
      // caller's token is concurrently released. This keeps retained contexts'
      // teardown outside the cache mutex.
      std::shared_ptr<context_map> retained;
      std::shared_ptr<Context> context;
      std::lock_guard lock(mutex);
      std::erase_if(contexts, [](const auto &entry) { return entry.second.expired(); });
      if (auto it = contexts.find(key); it != contexts.end()) {
        context = it->second.lock();
      }
      if (!context) {
        context = create();
        if (!context) return nullptr;
        contexts[key] = context;
      }
      retained = retention.lock();
      if (retained) {
        (*retained)[key] = context;
      }
      return context;
    }

    /**
     * @brief Forget a dead context without invalidating a newer replacement.
     *        Existing encoders still own it until their resources are cleaned up.
     */
    void
    erase(const std::shared_ptr<Context> &context) {
      std::shared_ptr<context_map> retained;
      std::lock_guard lock(mutex);
      retained = retention.lock();
      for (auto it = contexts.begin(); it != contexts.end();) {
        // Compare ownership without temporarily locking another encoder's
        // context, which could make this thread run its destructor under mutex.
        if (!it->second.owner_before(context) && !context.owner_before(it->second)) {
          if (retained) retained->erase(it->first);
          it = contexts.erase(it);
        }
        else {
          ++it;
        }
      }
    }

  private:
    using context_map = std::map<Key, std::shared_ptr<Context>>;
    std::mutex mutex;
    std::map<Key, std::weak_ptr<Context>> contexts;
    std::weak_ptr<context_map> retention;
  };

}  // namespace nvenc
