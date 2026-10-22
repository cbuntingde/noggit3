// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/quaternion.hpp>
#include <noggit/MPQ.h>
#include <noggit/ModelHeaders.h>

#include <cassert>
#include <cstring>
#include <map>
#include <vector>
#include <memory>

namespace Animation
{
  // pre-Wrath M2 (version < 264) keeps one flat timestamp/key array per
  // animation track, shared by every sequence on a single timeline; the
  // per-animation AnimationBlockHeader indirection (and .anim files) only
  // exist from WotLK on
  inline bool tracks_are_flat(const MPQFile& file)
  {
    if (file.getSize() < 8 || std::memcmp(file.getBuffer(), "MD20", 4) != 0)
    {
      return false;
    }

    char const* buffer = file.getBuffer();
    uint32_t version = static_cast<uint8_t>(buffer[4])
                     | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[5])) << 8)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[6])) << 16)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[7])) << 24);

    return version < 264;
  }
  namespace Interpolation
  {
    //! \todo C++0x: Change namespace to "enum class Type : int16_t", remove typedef.
    namespace Type
    {
      typedef int16_t Type_t;
      enum
      {
        NONE,
        LINEAR,
        HERMITE
      };
    }
  }

  template<class FROM, class TO>
  struct Conversion
  {
    inline TO operator()(const FROM& value)
    {
      return TO(value);
    }
  };

  template<>
  inline math::quaternion Conversion<math::packed_quaternion, math::quaternion>::operator()(const math::packed_quaternion& value)
  {
    //! \todo Check if this is really correct.
    return math::quaternion(
      static_cast<float>((value.x > 0 ? value.x - 32767 : value.x + 32767) / 32767.0f),
      static_cast<float>((value.y > 0 ? value.y - 32767 : value.y + 32767) / 32767.0f),
      static_cast<float>((value.z > 0 ? value.z - 32767 : value.z + 32767) / 32767.0f),
      static_cast<float>((value.w > 0 ? value.w - 32767 : value.w + 32767) / 32767.0f));
  }

  template<>
  inline float Conversion<int16_t, float>::operator()(const int16_t& value)
  {
    return value / 32767.0f;
  }

  //! \note AnimatedType is the type of data getting animated.
  //! \note DataType is the type of data stored.
  //! \note The conversion from DataType to AnimatedType is done via Animation::Conversion.
  template<class AnimatedType, class DataType = AnimatedType>
  class M2Value
  {
  private:
    typedef uint32_t TimestampType;
    typedef uint32_t AnimationIdType;

    typedef std::vector<AnimatedType> AnimatedTypeVectorType;
    typedef std::vector<TimestampType> TimestampTypeVectorType;

    Animation::Conversion<DataType, AnimatedType> _conversion;

    static const int32_t NO_GLOBAL_SEQUENCE = -1;
    int32_t _globalSequenceID;
    int32_t* _globalSequences;

    Animation::Interpolation::Type::Type_t _interpolationType;
    bool _flat_tracks = false;

    std::map<AnimationIdType, TimestampTypeVectorType> times;
    std::map<AnimationIdType, AnimatedTypeVectorType> data;

    // for nonlinear interpolations:
    std::map<AnimationIdType, AnimatedTypeVectorType> in;
    std::map<AnimationIdType, AnimatedTypeVectorType> out;

  public:
    bool uses(AnimationIdType anim)
    {
      if (_globalSequenceID != NO_GLOBAL_SEQUENCE)
      {
        anim = AnimationIdType();
      }

      return !data[anim].empty();
    }

    AnimatedType getValue (AnimationIdType anim, TimestampType time, int animtime)
    {
      if (_globalSequenceID != NO_GLOBAL_SEQUENCE)
      {
        if (_globalSequences[_globalSequenceID])
        {
          time = animtime % _globalSequences[_globalSequenceID];
        }
        else
        {
          time = TimestampType();
        }
        anim = AnimationIdType();
      }
      else if (_flat_tracks)
      {
        anim = AnimationIdType();
      }

      TimestampTypeVectorType& timestampVector = times[anim];
      AnimatedTypeVectorType& dataVector = data[anim];
      AnimatedTypeVectorType& inVector = in[anim];
      AnimatedTypeVectorType& outVector = out[anim];

      if (dataVector.empty())
      {
        return AnimatedType();
      }

      AnimatedType result = dataVector[0];

      if (!timestampVector.empty())
      {
        TimestampType max_time = timestampVector.back();
        if (max_time > 0)
        {
          time %= max_time;
        }
        else
        {
          time = TimestampType();
        }

        size_t pos = 0;
        for (size_t i = 0; i < timestampVector.size() - 1; ++i)
        {
          if (time >= timestampVector[i] && time < timestampVector[i + 1])
          {
            pos = i;
            break;
          }
        }

        if (pos == timestampVector.size() - 1 || _interpolationType == Animation::Interpolation::Type::NONE)
        {
          result = dataVector[pos];
        }
        else
        {
          TimestampType t1 = timestampVector[pos];
          TimestampType t2 = timestampVector[pos + 1];
          const float percentage = (time - t1) / static_cast<float>(t2 - t1);

          switch (_interpolationType)
          {
          case Animation::Interpolation::Type::LINEAR:
          {
            result = math::interpolation::linear (percentage, dataVector[pos], dataVector[pos + 1]);
          }
            break;

          case Animation::Interpolation::Type::HERMITE:
          {
            result = math::interpolation::hermite(percentage, dataVector[pos], dataVector[pos + 1], inVector[pos], outVector[pos]);
          }
            break;
          }
        }
      }

      return result;
    }

    //! \todo Use a vector of MPQFile& for the anim files instead for safety.
    M2Value (const AnimationBlock& animationBlock, const MPQFile& file, int32_t* globalSequences, const std::vector<std::unique_ptr<MPQFile>>& animation_files = std::vector<std::unique_ptr<MPQFile>>())
    {
      assert(animationBlock.nTimes == animationBlock.nKeys);

      _interpolationType = animationBlock.type;

      _globalSequences = globalSequences;
      _globalSequenceID = animationBlock.seq;
      if (_globalSequenceID != NO_GLOBAL_SEQUENCE)
      {
        assert(_globalSequences && "Animation said to have global sequence, but pointer to global sequence data is nullptr");
      }

      _flat_tracks = tracks_are_flat(file);

      // pre-Wrath tracks hold the timestamps/keys directly, all sequences
      // sharing one timeline; WotLK indirection uses one header per animation
      size_t const timestamp_block_count = _flat_tracks ? 1 : animationBlock.nTimes;
      size_t const key_block_count = _flat_tracks ? 1 : animationBlock.nKeys;

      const AnimationBlockHeader* timestampHeaders = _flat_tracks ? nullptr : file.get_array<AnimationBlockHeader>(animationBlock.ofsTimes, animationBlock.nTimes);
      const AnimationBlockHeader* keyHeaders = _flat_tracks ? nullptr : file.get_array<AnimationBlockHeader>(animationBlock.ofsKeys, animationBlock.nKeys);

      for (size_t j = 0; j < timestamp_block_count; ++j)
      {
        size_t const count = _flat_tracks ? animationBlock.nTimes : timestampHeaders[j].nEntries;
        size_t const ofs = _flat_tracks ? animationBlock.ofsTimes : timestampHeaders[j].ofsEntries;

        // The whole timestamp array has to fit inside the file. Reading it
        // element by element from the raw buffer would happily run past the
        // end of it for a malformed file.
        std::vector<TimestampType> timestamps (count);
        if (j < animation_files.size() && animation_files[j])
          animation_files[j]->read_array<TimestampType>(ofs, count, timestamps.data());
        else
          file.read_array<TimestampType>(ofs, count, timestamps.data());

        for (size_t i = 0; i < count; ++i)
        {
          times[j].push_back(timestamps[i]);
        }
      }

      for (size_t j = 0; j < key_block_count; ++j)
      {
        size_t const count = _flat_tracks ? animationBlock.nKeys : keyHeaders[j].nEntries;
        size_t const ofs = _flat_tracks ? animationBlock.ofsKeys : keyHeaders[j].ofsEntries;

        // hermite tracks store three keys per timestamp
        size_t const key_count = (_interpolationType == Animation::Interpolation::Type::HERMITE) ? count * 3 : count;

        std::vector<DataType> keys (key_count);
        if (j < animation_files.size() && animation_files[j])
          animation_files[j]->read_array<DataType>(ofs, key_count, keys.data());
        else
          file.read_array<DataType>(ofs, key_count, keys.data());

        switch (_interpolationType)
        {
        case Animation::Interpolation::Type::NONE:
        case Animation::Interpolation::Type::LINEAR:
          for (size_t i = 0; i < count; ++i)
          {
            data[j].push_back(_conversion(keys[i]));
          }
          break;

        case Animation::Interpolation::Type::HERMITE:
          for (size_t i = 0; i < count; ++i)
          {
            data[j].push_back(_conversion(keys[i * 3]));
            in[j].push_back(_conversion(keys[i * 3 + 1]));
            out[j].push_back(_conversion(keys[i * 3 + 2]));
          }
          break;
        }
      }
    }

    void apply(AnimatedType function(const AnimatedType))
    {
      switch (_interpolationType)
      {
      case Animation::Interpolation::Type::NONE:
      case Animation::Interpolation::Type::LINEAR:
        for (size_t i = 0; i < data.size(); ++i)
        {
          for (size_t j = 0; j < data[i].size(); ++j)
          {
            data[i][j] = function(data[i][j]);
          }
        }
        break;

      case Animation::Interpolation::Type::HERMITE:
        for (size_t i = 0; i < data.size(); ++i)
        {
          for (size_t j = 0; j < data[i].size(); ++j)
          {
            data[i][j] = function(data[i][j]);
            in[i][j] = function(in[i][j]);
            out[i][j] = function(out[i][j]);
          }
        }
        break;
      }
    }
  };
};
