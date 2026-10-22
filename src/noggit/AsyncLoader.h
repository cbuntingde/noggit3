// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/AsyncObject.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <thread>

class AsyncLoader
{
public:
  // use regular pointer because unique_ptr was causing
  // a significant performance hit
  static AsyncLoader* instance;

  static void setup(int threads);
  //! Stop the loader threads and release the instance. Has to be called
  //! before the objects the loader works on are destroyed.
  static void shutdown();

  //! Ownership is _not_ transferred. Call ensure_deletable to ensure
  //! that a previously enqueued object can be destroyed.
  void queue_for_load (AsyncObject*);

  void ensure_deletable (AsyncObject*);

  // wait until everything is loaded
  void wait_queue_empty();

  AsyncLoader(int numThreads);
  ~AsyncLoader();

  bool important_object_failed_loading() const { return _important_object_failed_loading; }
  void reset_object_fail() { _important_object_failed_loading = false; }

private:
  void process();

  std::mutex _guard;
  std::condition_variable _state_changed;
  std::atomic<bool> _stop;
  std::atomic<int> _object_queued_count = { 0 };
  std::array<std::list<AsyncObject*>, (size_t)async_priority::count> _to_load;
  std::list<AsyncObject*> _currently_loading;
  std::list<std::thread> _threads;
  bool _important_object_failed_loading = false;
};
