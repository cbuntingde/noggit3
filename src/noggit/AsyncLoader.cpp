// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/errorHandling.h>
#include <noggit/settings.hpp>

#include <algorithm>
#include <list>

AsyncLoader* AsyncLoader::instance;

void AsyncLoader::setup(int threads)
{
  // make sure there's always at least
  // one thread otherwise nothing can load
  instance = new AsyncLoader(std::max(1, threads));
}

void AsyncLoader::process()
{
  AsyncObject* object = nullptr;

  bool additional_log = NoggitSettings.value("additional_file_loading_log", false).toBool();

  while (!_stop)
  {
    {
      std::unique_lock<std::mutex> lock (_guard);

      _state_changed.wait
      ( lock
      , [&]
        {
          return !!_stop || std::any_of ( _to_load.begin(), _to_load.end()
                                        , [](auto const& to_load) { return !to_load.empty(); }
                                        );
        }
      );

      if (_stop)
      {
        return;
      }

      for (auto& to_load : _to_load)
      {
        if (to_load.empty())
        {
          continue;
        }

        object = to_load.front();
        _currently_loading.emplace_back (object);
        to_load.pop_front();

        break;
      }
    }

    try
    {
      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loading '" << object->filename << "'" << std::endl;
      }

      object->finishLoading();

      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loaded  '" << object->filename << "'" << std::endl;
      }

      {
        std::lock_guard<std::mutex> const lock (_guard);
        _currently_loading.remove (object);
        _state_changed.notify_all();

        _object_queued_count--;
      }
    }
    catch (...)
    {
      std::lock_guard<std::mutex> const lock(_guard);
      object->error_on_loading();

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      // a failed load has to release its slot like a successful one,
      // otherwise the object stays in _currently_loading forever and
      // ensure_deletable/wait_queue_empty wait on it endlessly
      _currently_loading.remove (object);
      _object_queued_count--;
      _state_changed.notify_all();
    }
  }
}

void AsyncLoader::queue_for_load (AsyncObject* object)
{
  std::lock_guard<std::mutex> const lock (_guard);
  _to_load[(size_t)object->loading_priority()].push_back (object);
  _state_changed.notify_one();

  _object_queued_count++;
}

void AsyncLoader::ensure_deletable (AsyncObject* object)
{
  std::unique_lock<std::mutex> lock (_guard);
  _state_changed.wait
  ( lock
  , [&]
    {
      auto& to_load = _to_load[(size_t)object->loading_priority()];
      auto const& it = std::find (to_load.begin(), to_load.end(), object);

      // don't load it if it's just to delete it afterward
      if (it != to_load.end())
      {
        to_load.erase(it);
        return true;
      }
      else
      {
        return std::find (_currently_loading.begin(), _currently_loading.end(), object) == _currently_loading.end();
      }
    }
  );
}

void AsyncLoader::wait_queue_empty()
{
  std::unique_lock<std::mutex> lock (_guard);
  _state_changed.wait
  ( lock
  , [&]
    {
      return _object_queued_count.load() == 0;
    }
  );
}

AsyncLoader::AsyncLoader(int numThreads)
  : _stop (false)
{
  for (int i = 0; i < numThreads; ++i)
  {
    _threads.emplace_back (&AsyncLoader::process, this);
  }
}

AsyncLoader::~AsyncLoader()
{
  _stop = true;
  _state_changed.notify_all();

  for (auto& thread : _threads)
  {
    thread.join();
  }
}
