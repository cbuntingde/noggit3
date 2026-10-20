// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/Log.h>
#include <noggit/MPQ.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace noggit
{
  template<typename T>
    struct async_object_multimap_with_normalized_key
  {
    async_object_multimap_with_normalized_key (std::function<std::string (std::string)> normalize = &mpq::normalized_filename)
      : _normalize (std::move (normalize))
    {}

    ~async_object_multimap_with_normalized_key()
    {
      apply ( [&] (std::string const& key, T const&)
              {
                LogDebug << key << ": " << _counts.at (key) << std::endl;
              }
            );
    }

    template<typename... Args>
      T* emplace (std::string const& filename, Args&&... args)
    {
      std::string const normalized (_normalize (filename));

      T* obj = nullptr;
      bool const is_new = [&]
      {
        std::scoped_lock lock(_mutex);

        auto count_it (_counts.find (normalized));
        if (count_it != _counts.end() && count_it->second > 0)
        {
          ++count_it->second;
          auto element_it (_elements.find (normalized));
          // Invariant: a positive count implies a live element.
          // Repair instead of throwing when the maps disagree.
          if (element_it == _elements.end())
          {
            auto emplaced (_elements.emplace ( std::piecewise_construct
                                            , std::forward_as_tuple (normalized)
                                            , std::forward_as_tuple (normalized, args...)
                                            ));
            count_it->second = 1;
            obj = &emplaced.first->second;
            return true;
          }
          obj = &element_it->second;
          return false;
        }

        auto emplaced (_elements.emplace ( std::piecewise_construct
                                          , std::forward_as_tuple (normalized)
                                          , std::forward_as_tuple (normalized, args...)
                                          ));
        _counts[normalized] = 1;
        obj = &emplaced.first->second;
        return true;
      }();

      if (is_new && AsyncLoader::instance)
      {
        AsyncLoader::instance->queue_for_load(static_cast<AsyncObject*>(obj));
      }

      return obj;
    }
    void erase (std::string const& filename)
    {
      std::string const normalized (_normalize (filename));
      AsyncObject* obj = nullptr;

      {
        std::scoped_lock lock(_mutex);

        auto count_it (_counts.find (normalized));
        if (count_it == _counts.end() || count_it->second == 0)
        {
          return;
        }

        if (--count_it->second != 0)
        {
          return;
        }

        auto element_it (_elements.find (normalized));
        if (element_it == _elements.end())
        {
          _counts.erase (count_it);
          return;
        }

        obj = static_cast<AsyncObject*>(&element_it->second);
      }

      // always make sure an async object can be deleted before deleting it
      if (AsyncLoader::instance && !obj->finishedLoading())
      {
        AsyncLoader::instance->ensure_deletable(obj);
      }

      {
        std::scoped_lock lock(_mutex);
        _elements.erase (normalized);
        _counts.erase (normalized);
      }
    }
    void apply (std::function<void (std::string const&, T&)> fun)
    {
      std::scoped_lock lock(_mutex);
      for (auto& element : _elements)
      {
        fun (element.first, element.second);
      }
    }
    void apply (std::function<void (std::string const&, T const&)> fun) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        fun (element.first, element.second);
      }
    }

  private:
    std::map<std::string, T> _elements;
    std::unordered_map<std::string, std::size_t> _counts;
    std::function<std::string (std::string)> _normalize;
    std::mutex _mutex;
  };
}
