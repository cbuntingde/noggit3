// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/adt_roundtrip.hpp>

#include <noggit/Log.h>
#include <noggit/MPQ.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/settings.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  struct adt_bytes
  {
    std::vector<char> buffer;

    std::size_t size() const { return buffer.size(); }

    std::uint64_t hash() const
    {
      // FNV-1a
      std::uint64_t hash (14695981039346656037ull);

      for (char c : buffer)
      {
        hash ^= static_cast<std::uint8_t> (c);
        hash *= 1099511628211ull;
      }

      return hash;
    }

    bool read_uint32_at (std::size_t offset, std::uint32_t& out) const
    {
      if (offset + 4 > buffer.size())
      {
        return false;
      }

      std::memcpy (&out, buffer.data() + offset, 4);
      return true;
    }

    // fourccs are stored byte-reversed on disk, e.g. "KNCM" for MCNK
    std::size_t fourcc_count (char const fourcc[4]) const
    {
      std::size_t count (0);

      for (std::size_t i (0); i + 4 <= buffer.size(); ++i)
      {
        if (!std::memcmp (buffer.data() + i, fourcc, 4))
        {
          ++count;
        }
      }

      return count;
    }
  };

  struct tile_stats
  {
    std::size_t liquid_chunks = 0;  // chunks with sizeLiquid > 8
    std::size_t liquid_bytes = 0;   // sum of sizeLiquid - 8
    std::size_t doodads = 0;        // MDDF entries
    std::size_t wmos = 0;           // MODF entries
  };

  // measures directly on the raw bytes so original and saved files can be
  // compared even when the loader or saver misbehaves
  tile_stats measure_tile (adt_bytes const& bytes)
  {
    tile_stats stats;

    // MapChunkHeader starts after the MCNK chunk header, sizeLiquid at 0x64
    for (std::size_t i (0); i + 8 + 0x80 <= bytes.size(); ++i)
    {
      if (std::memcmp (bytes.buffer.data() + i, "KNCM", 4))
      {
        continue;
      }

      std::uint32_t size_liquid (0);

      if (bytes.read_uint32_at (i + 8 + 0x64, size_liquid) && size_liquid > 8)
      {
        ++stats.liquid_chunks;
        stats.liquid_bytes += size_liquid - 8;
      }
    }

    // MHDR data starts at 0x14, mddf at 0x1C and modf at 0x20 in it;
    // both chunk offsets are relative to the MHDR data start
    std::uint32_t mhdr_mddf (0);
    std::uint32_t mhdr_modf (0);

    if (bytes.read_uint32_at (0x14 + 0x1C, mhdr_mddf) && mhdr_mddf)
    {
      std::uint32_t entry_size (0);

      if (bytes.read_uint32_at (0x14 + mhdr_mddf + 4, entry_size))
      {
        stats.doodads = entry_size / 0x24;
      }
    }

    if (bytes.read_uint32_at (0x14 + 0x20, mhdr_modf) && mhdr_modf)
    {
      std::uint32_t entry_size (0);

      if (bytes.read_uint32_at (0x14 + mhdr_modf + 4, entry_size))
      {
        stats.wmos = entry_size / 0x40;
      }
    }

    return stats;
  }

  bool read_file (std::filesystem::path const& path, adt_bytes& out)
  {
    std::ifstream input (path.string(), std::ios_base::binary | std::ios_base::in);

    if (!input.is_open())
    {
      return false;
    }

    input.seekg (0, std::ios::end);
    out.buffer.resize (static_cast<std::size_t> (input.tellg()));
    input.seekg (0, std::ios::beg);
    input.read (out.buffer.data(), static_cast<std::streamsize> (out.buffer.size()));

    return true;
  }

  bool read_original (std::string const& filename, adt_bytes& out)
  {
    try
    {
      MPQFile file (filename);
      out.buffer.assign (file.getBuffer(), file.getBuffer() + file.getSize());
      return true;
    }
    catch (std::exception const& e)
    {
      LogError << "adt_roundtrip: could not read \"" << filename << "\": " << e.what() << std::endl;
      return false;
    }
  }

  int round_trip_tiles ( std::string const& map_name
                       , int map_id
                       , std::filesystem::path const& output_folder
                       , int only_x
                       , int only_z
                       )
  {
    World world (map_name, map_id);

    MapIndex& index (world.mapIndex);

    bool const big_alpha (index.hasBigAlpha());
    bool const green_lava (index.use_mclq_green_lava());

    std::size_t tiles_total (0);
    std::size_t tiles_ok (0);
    std::vector<std::string> failed_tiles;

    for (int z (0); z < 64; ++z)
    {
      for (int x (0); x < 64; ++x)
      {
        if (only_x >= 0 && (x != only_x || z != only_z))
        {
          continue;
        }

        if (!index.hasTile (tile_index (x, z)))
        {
          continue;
        }

        ++tiles_total;

        std::stringstream filename;
        filename << "World\\Maps\\" << map_name << "\\" << map_name << "_" << x << "_" << z << ".adt";
        std::string const adt_filename (filename.str());
        std::filesystem::path const saved_path (output_folder / noggit::mpq::normalized_filename (adt_filename));

        NOGGIT_LOG << "adt_roundtrip: tile " << x << "-" << z << ": loading original..." << std::endl;

        adt_bytes original;
        if (!read_original (adt_filename, original))
        {
          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (could not read the original)");
          continue;
        }

        // dump the original next to the saved versions so a failing tile
        // can be compared against the file it was loaded from
        std::filesystem::path const original_dump_path (saved_path.string() + ".orig");
        {
          std::ofstream dump (original_dump_path.string(), std::ios_base::binary | std::ios_base::trunc);
          dump.write (original.buffer.data(), static_cast<std::streamsize> (original.buffer.size()));
        }

        tile_stats const original_stats (measure_tile (original));

        // load the original and save it into the scratch folder
        try
        {
          MapTile tile (x, z, adt_filename, big_alpha, false, green_lava, false, &world, tile_mode::uid_fix_all);
          tile.finishLoading();
          tile.saveTile (&world);
        }
        catch (std::exception const& e)
        {
          LogError << "adt_roundtrip: tile " << x << "-" << z << ": load/save of the original failed: " << e.what() << std::endl;
          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (load/save failed)");
          continue;
        }

        adt_bytes saved;
        if (!read_file (saved_path, saved))
        {
          LogError << "adt_roundtrip: tile " << x << "-" << z << ": no file was written to \"" << saved_path.string() << "\"" << std::endl;
          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (nothing saved)");
          continue;
        }

        // keep the first saved version around so that a failing tile
        // leaves both generations on disk for byte-level inspection
        std::filesystem::path const first_saved_path (saved_path.string() + ".first");
        {
          std::error_code copy_error;
          std::filesystem::copy_file
            (saved_path, first_saved_path
            , std::filesystem::copy_options::overwrite_existing, copy_error
            );
        }

        // load the saved file back (it shadows the original) and save it again
        try
        {
          MapTile tile (x, z, adt_filename, big_alpha, false, green_lava, false, &world, tile_mode::uid_fix_all);
          tile.finishLoading();
          tile.saveTile (&world);
        }
        catch (std::exception const& e)
        {
          LogError << "adt_roundtrip: tile " << x << "-" << z << ": reloading the saved file failed: " << e.what() << std::endl;
          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (reload failed, saved file kept)");
          continue;
        }

        adt_bytes resaved;
        if (!read_file (saved_path, resaved))
        {
          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (resave missing)");
          continue;
        }

        std::size_t const mcnk_count (resaved.fourcc_count ("KNCM"));
        tile_stats const saved_stats (measure_tile (resaved));

        bool const chunks_ok (mcnk_count == 256);
        bool const liquid_ok ( saved_stats.liquid_chunks == original_stats.liquid_chunks
                             && saved_stats.liquid_bytes == original_stats.liquid_bytes
                             );
        bool const stable (saved.hash() == resaved.hash());

        if (chunks_ok && liquid_ok && stable)
        {
          ++tiles_ok;

          NOGGIT_LOG << "adt_roundtrip: tile " << x << "-" << z << " ok"
            << " (" << original.size() << " -> " << saved.size() << " bytes, "
            << original_stats.liquid_chunks << " liquid chunks preserved, "
            << original_stats.doodads << " m2 + " << original_stats.wmos << " wmo (models are not written in this mode))"
            << std::endl;

          std::error_code cleanup_error;
          std::filesystem::remove (saved_path, cleanup_error);
          std::filesystem::remove (first_saved_path, cleanup_error);
          std::filesystem::remove (original_dump_path, cleanup_error);
        }
        else
        {
          if (!chunks_ok)
          {
            LogError << "adt_roundtrip: tile " << x << "-" << z << ": saved file has " << mcnk_count
              << " MCNK chunks, expected 256" << std::endl;
          }

          if (!liquid_ok)
          {
            LogError << "adt_roundtrip: tile " << x << "-" << z << ": liquid changed: "
              << original_stats.liquid_chunks << " -> " << saved_stats.liquid_chunks << " chunks, "
              << original_stats.liquid_bytes << " -> " << saved_stats.liquid_bytes << " payload bytes"
              << std::endl;
          }

          if (!stable)
          {
            LogError << "adt_roundtrip: tile " << x << "-" << z << ": saving the reloaded file is not stable ("
              << saved.hash() << " -> " << resaved.hash() << ")" << std::endl;
          }

          failed_tiles.push_back (std::to_string (x) + "-" + std::to_string (z) + " (saved file kept)");
        }
      }
    }

    NOGGIT_LOG << "adt_roundtrip: " << tiles_ok << "/" << tiles_total << " tiles round-tripped" << std::endl;

    for (auto const& failure : failed_tiles)
    {
      NOGGIT_LOG << "adt_roundtrip: failed: " << failure << std::endl;
    }

    return failed_tiles.empty() ? 0 : 1;
  }
}

namespace noggit
{
  int run_adt_roundtrip (std::string const& map_name, int map_id, int tile_x, int tile_z)
  {
    NOGGIT_LOG << "adt_roundtrip: map " << map_id << " (\"" << map_name << "\")"
      << (tile_x >= 0 && tile_z >= 0
            ? " tile " + std::to_string (tile_x) + "-" + std::to_string (tile_z)
            : " (all tiles)")
      << std::endl;

    std::filesystem::path const output_folder (std::filesystem::current_path() / "adt_roundtrip" / map_name);

    std::error_code error;
    std::filesystem::remove_all (output_folder, error);
    std::filesystem::create_directories (output_folder, error);

    if (error)
    {
      LogError << "adt_roundtrip: could not create \"" << output_folder.string() << "\": " << error.message() << std::endl;
      return 1;
    }

    // the save path always writes into the configured project folder and
    // mclq export folder; redirect both to the scratch folder while running
    QString const original_project_path (NoggitSettings.value ("project/path").toString());
    QVariant const original_mclq_path (NoggitSettings.value ("project/mclq_liquids_path"));
    QVariant const original_mclq_export (NoggitSettings.value ("use_mclq_liquids_export"));

    NoggitSettings.set_value ("project/path", QString::fromStdString (output_folder.string()) + "/");
    NoggitSettings.set_value ("project/mclq_liquids_path", QString());
    NoggitSettings.set_value ("use_mclq_liquids_export", true);
    NoggitSettings.sync();

    int result (1);

    try
    {
      result = round_trip_tiles (map_name, map_id, output_folder, tile_x, tile_z);
    }
    catch (std::exception const& e)
    {
      LogError << "adt_roundtrip: aborted: " << e.what() << std::endl;
    }

    NoggitSettings.set_value ("project/path", original_project_path);
    NoggitSettings.set_value ("project/mclq_liquids_path", original_mclq_path);
    NoggitSettings.set_value ("use_mclq_liquids_export", original_mclq_export);
    NoggitSettings.sync();

    NOGGIT_LOG << "adt_roundtrip: done with exit code " << result << std::endl;

    return result;
  }
}
