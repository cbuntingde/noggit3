// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/adt_roundtrip.hpp>

#include <noggit/Log.h>
#include <noggit/MPQ.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/settings.hpp>

#include <algorithm>
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

  // mcnk liquid layout (mclq, MapHeaders.h): the chunk header sits 8 bytes
  // after the fourcc with its liquid fields at 0x60/0x64; the mclq fourcc
  // is ofsLiquid bytes after the mcnk fourcc and the payload follows its
  // 8 byte sub-chunk header
  static_assert (sizeof (mclq) == 804, "mclq layout changed, the byte offsets used by the liquid metrics need updating");

  struct mcnk_liquid
  {
    std::uint32_t flags;        // mcnk flags word, bits 2-5 name the liquid layers
    std::uint32_t payload_size; // mclq data bytes (sizeLiquid - 8)
    std::size_t payload_offset; // first mclq data byte in the file
  };

  // one entry per MCNK chunk, in file order; chunks are found by fourcc and
  // skipped by their size so no sub-chunk data can be misread as a chunk
  std::vector<mcnk_liquid> scan_mcnk_liquids (adt_bytes const& bytes)
  {
    std::vector<mcnk_liquid> chunks;

    std::size_t i (0);

    while (i + 8 + 0x80 <= bytes.size())
    {
      if (std::memcmp (bytes.buffer.data() + i, "KNCM", 4))
      {
        ++i;
        continue;
      }

      std::uint32_t chunk_size (0);
      std::uint32_t flags (0);
      std::uint32_t ofs_liquid (0);
      std::uint32_t size_liquid (0);

      if ( !bytes.read_uint32_at (i + 4, chunk_size)
        || !bytes.read_uint32_at (i + 8, flags)
        || !bytes.read_uint32_at (i + 8 + 0x60, ofs_liquid)
        || !bytes.read_uint32_at (i + 8 + 0x64, size_liquid))
      {
        break;
      }

      mcnk_liquid chunk;
      chunk.flags = flags;
      chunk.payload_size = size_liquid > 8 ? size_liquid - 8 : 0;
      chunk.payload_offset = i + ofs_liquid + 8;
      chunks.push_back (chunk);

      // a sane mcnk size covers at least its 0x80 byte header; anything
      // else means the size cannot be trusted, so fall back to scanning
      i += chunk_size >= 0x80 ? 8 + chunk_size : 1;
    }

    return chunks;
  }

  // everything the drift-prone liquid fields hold, counted per file so the
  // original can be compared against what the editor wrote
  struct liquid_census
  {
    std::size_t chunks = 0;              // MCNK chunks seen
    std::size_t liquid_chunks = 0;       // chunks carrying an mclq payload
    std::size_t layers = 0;              // mclq payloads (804 bytes each)
    std::size_t malformed_payloads = 0;  // payload size not a multiple of sizeof(mclq)
    std::size_t lq_flag_chunks = 0;      // mcnk flags naming at least one liquid layer
    std::size_t tile_types[8] = {};      // per liquid type across all tiles
    std::size_t dont_render_tiles = 0;
    std::size_t tiles_with_bits_4_5 = 0; // flag_0x10 / flag_0x20
    std::size_t fishable_tiles = 0;
    std::size_t fatigue_tiles = 0;
    std::size_t flow_vertices = 0;       // vertices with a nonzero flow percentage
    std::size_t flow_layers = 0;         // layers with n_flowvs or flow data
  };

  liquid_census census_liquids (adt_bytes const& bytes)
  {
    liquid_census census;

    for (auto const& chunk : scan_mcnk_liquids (bytes))
    {
      ++census.chunks;

      if (chunk.flags & 0x3C)
      {
        ++census.lq_flag_chunks;
      }

      if (!chunk.payload_size)
      {
        continue;
      }

      ++census.liquid_chunks;

      if ( chunk.payload_size % sizeof (mclq)
        || chunk.payload_offset + chunk.payload_size > bytes.size())
      {
        ++census.malformed_payloads;
        continue;
      }

      std::size_t const layers (chunk.payload_size / sizeof (mclq));
      census.layers += layers;

      char const* data (bytes.buffer.data() + chunk.payload_offset);

      for (std::size_t layer (0); layer < layers; ++layer)
      {
        mclq liquid;
        std::memcpy (&liquid, data + layer * sizeof (mclq), sizeof (mclq));

        // for magma the vertex bytes hold uv coordinates instead of flows
        bool const magma (liquid.tiles[0].liquid_type == 6);

        for (auto const& tile : liquid.tiles)
        {
          ++census.tile_types[tile.liquid_type];

          census.dont_render_tiles += tile.dont_render;
          census.tiles_with_bits_4_5 += tile.flag_0x10 || tile.flag_0x20;
          census.fishable_tiles += tile.fishable;
          census.fatigue_tiles += tile.fatigue;
        }

        for (auto const& vertex : liquid.vertices)
        {
          if (!magma && (vertex.water.flow_0_pct || vertex.water.flow_1_pct))
          {
            ++census.flow_vertices;
          }
        }

        bool flow_data (liquid.n_flowvs != 0);

        for (std::size_t b (0); !flow_data && b < sizeof (liquid.flowvs); ++b)
        {
          flow_data = reinterpret_cast<char const*> (liquid.flowvs)[b] != 0;
        }

        census.flow_layers += flow_data;
      }
    }

    return census;
  }

  // how far the first save drifts from the original file, per mclq region
  struct liquid_fidelity
  {
    std::size_t chunks_compared = 0;
    std::size_t layers_compared = 0;
    std::size_t byte_identical = 0;
    std::size_t size_mismatch = 0;      // same chunk, different payload size
    std::size_t minmax_diff = 0;        // min_height / max_height
    std::size_t height_diff = 0;        // per vertex heights
    std::size_t depth_flow_diff = 0;    // per vertex depth / magma uv / flow bytes
    std::size_t tiles_type_diff = 0;    // tile liquid type + dont_render (low nibble)
    std::size_t tiles_b45_diff = 0;     // tile flag_0x10 / flag_0x20
    std::size_t tiles_ff_diff = 0;      // tile fishable / fatigue
    std::size_t flowdata_diff = 0;      // n_flowvs + flowvs data
    std::size_t flags_diff = 0;         // mcnk flags word
    std::size_t lq_flags_diff = 0;      // liquid layer bits (2-5) of the flags word
    std::size_t flags_bits_diff[32] = {}; // per bit of the flags word
    std::size_t only_original = 0;      // chunks present in only one of the files
    std::size_t only_saved = 0;
  };

  liquid_fidelity compare_liquids (adt_bytes const& original, adt_bytes const& saved)
  {
    liquid_fidelity fidelity;

    auto const original_chunks (scan_mcnk_liquids (original));
    auto const saved_chunks (scan_mcnk_liquids (saved));

    if (original_chunks.size() > saved_chunks.size())
    {
      fidelity.only_original = original_chunks.size() - saved_chunks.size();
    }
    else
    {
      fidelity.only_saved = saved_chunks.size() - original_chunks.size();
    }

    std::size_t const common (std::min (original_chunks.size(), saved_chunks.size()));

    for (std::size_t i (0); i < common; ++i)
    {
      mcnk_liquid const& a (original_chunks[i]);
      mcnk_liquid const& b (saved_chunks[i]);

      if (a.flags != b.flags)
      {
        ++fidelity.flags_diff;
        fidelity.lq_flags_diff += (a.flags & 0x3C) != (b.flags & 0x3C);

        std::uint32_t const changed (a.flags ^ b.flags);

        for (int bit (0); bit < 32; ++bit)
        {
          fidelity.flags_bits_diff[bit] += (changed >> bit) & 1;
        }
      }

      // both sides empty is the healthy case for a liquid-free chunk
      if (!a.payload_size && !b.payload_size)
      {
        continue;
      }

      ++fidelity.chunks_compared;

      if ( a.payload_size != b.payload_size
        || a.payload_offset + a.payload_size > original.size()
        || b.payload_offset + b.payload_size > saved.size())
      {
        ++fidelity.size_mismatch;
        continue;
      }

      char const* a_data (original.buffer.data() + a.payload_offset);
      char const* b_data (saved.buffer.data() + b.payload_offset);

      std::size_t const layers (a.payload_size / sizeof (mclq));
      fidelity.layers_compared += layers;

      bool identical (true);
      bool minmax (false);
      bool heights (false);
      bool depth_flow (false);
      bool tiles_type (false);
      bool tiles_b45 (false);
      bool tiles_ff (false);
      bool flowdata (false);

      for (std::size_t layer (0); layer < layers; ++layer)
      {
        char const* a_layer (a_data + layer * sizeof (mclq));
        char const* b_layer (b_data + layer * sizeof (mclq));

        if (!std::memcmp (a_layer, b_layer, sizeof (mclq)))
        {
          continue;
        }

        identical = false;

        if (std::memcmp (a_layer, b_layer, 8))
        {
          minmax = true;
        }

        for (int v (0); v < 81; ++v)
        {
          char const* a_vertex (a_layer + 8 + v * 8);
          char const* b_vertex (b_layer + 8 + v * 8);

          if (std::memcmp (a_vertex, b_vertex, 4))
          {
            depth_flow = true;
          }

          if (std::memcmp (a_vertex + 4, b_vertex + 4, 4))
          {
            heights = true;
          }
        }

        for (int t (0); t < 64; ++t)
        {
          std::uint8_t const a_tile (static_cast<std::uint8_t> (a_layer[8 + 648 + t]));
          std::uint8_t const b_tile (static_cast<std::uint8_t> (b_layer[8 + 648 + t]));

          tiles_type |= (a_tile & 0x0F) != (b_tile & 0x0F);
          tiles_b45 |= (a_tile & 0x30) != (b_tile & 0x30);
          tiles_ff |= (a_tile & 0xC0) != (b_tile & 0xC0);
        }

        // n_flowvs (4 bytes) + flowvs (2 * 40 bytes)
        flowdata |= std::memcmp (a_layer + 720, b_layer + 720, 84) != 0;
      }

      if (identical)
      {
        ++fidelity.byte_identical;
        continue;
      }

      fidelity.minmax_diff += minmax;
      fidelity.height_diff += heights;
      fidelity.depth_flow_diff += depth_flow;
      fidelity.tiles_type_diff += tiles_type;
      fidelity.tiles_b45_diff += tiles_b45;
      fidelity.tiles_ff_diff += tiles_ff;
      fidelity.flowdata_diff += flowdata;
    }

    return fidelity;
  }

  void merge_census (liquid_census& into, liquid_census const& from)
  {
    into.chunks += from.chunks;
    into.liquid_chunks += from.liquid_chunks;
    into.layers += from.layers;
    into.malformed_payloads += from.malformed_payloads;
    into.lq_flag_chunks += from.lq_flag_chunks;
    into.dont_render_tiles += from.dont_render_tiles;
    into.tiles_with_bits_4_5 += from.tiles_with_bits_4_5;
    into.fishable_tiles += from.fishable_tiles;
    into.fatigue_tiles += from.fatigue_tiles;
    into.flow_vertices += from.flow_vertices;
    into.flow_layers += from.flow_layers;

    for (int i (0); i < 8; ++i)
    {
      into.tile_types[i] += from.tile_types[i];
    }
  }

  void merge_fidelity (liquid_fidelity& into, liquid_fidelity const& from)
  {
    into.chunks_compared += from.chunks_compared;
    into.layers_compared += from.layers_compared;
    into.byte_identical += from.byte_identical;
    into.size_mismatch += from.size_mismatch;
    into.minmax_diff += from.minmax_diff;
    into.height_diff += from.height_diff;
    into.depth_flow_diff += from.depth_flow_diff;
    into.tiles_type_diff += from.tiles_type_diff;
    into.tiles_b45_diff += from.tiles_b45_diff;
    into.tiles_ff_diff += from.tiles_ff_diff;
    into.flowdata_diff += from.flowdata_diff;
    into.flags_diff += from.flags_diff;
    into.lq_flags_diff += from.lq_flags_diff;

    for (int bit (0); bit < 32; ++bit)
    {
      into.flags_bits_diff[bit] += from.flags_bits_diff[bit];
    }

    into.only_original += from.only_original;
    into.only_saved += from.only_saved;
  }

  void log_fidelity (std::string const& label, liquid_fidelity const& fidelity)
  {
    NOGGIT_LOG << "adt_roundtrip: liquid fidelity (" << label << "): "
      << fidelity.chunks_compared << " chunks / " << fidelity.layers_compared << " layers compared, "
      << fidelity.byte_identical << " byte-identical, "
      << fidelity.size_mismatch << " size mismatches, "
      << fidelity.minmax_diff << " min/max, "
      << fidelity.height_diff << " heights, "
      << fidelity.depth_flow_diff << " depth/flow bytes, "
      << fidelity.tiles_type_diff << " tile types, "
      << fidelity.tiles_b45_diff << " tile bits 4-5, "
      << fidelity.tiles_ff_diff << " tile bits 6-7, "
      << fidelity.flowdata_diff << " flow data, "
      << fidelity.flags_diff << " mcnk flags, "
      << fidelity.lq_flags_diff << " liquid layer flags, "
      << fidelity.only_original << " only in original, "
      << fidelity.only_saved << " only in saved"
      << std::endl;

    std::stringstream bits;

    for (int bit (0); bit < 32; ++bit)
    {
      if (fidelity.flags_bits_diff[bit])
      {
        bits << " bit" << bit << "=" << fidelity.flags_bits_diff[bit];
      }
    }

    if (!bits.str().empty())
    {
      NOGGIT_LOG << "adt_roundtrip: liquid fidelity (" << label << "): changed mcnk flag bits:" << bits.str() << std::endl;
    }
  }

  void log_census (std::string const& label, liquid_census const& census)
  {
    std::stringstream types;

    for (int i (0); i < 8; ++i)
    {
      if (census.tile_types[i])
      {
        types << " type" << i << "=" << census.tile_types[i];
      }
    }

    NOGGIT_LOG << "adt_roundtrip: liquid census (" << label << "): "
      << census.chunks << " mcnk, "
      << census.liquid_chunks << " with mclq, "
      << census.layers << " layers, "
      << census.malformed_payloads << " malformed payloads, "
      << census.lq_flag_chunks << " chunks with layer flags,"
      << types.str()
      << ", " << census.dont_render_tiles << " dont-render tiles"
      << ", " << census.tiles_with_bits_4_5 << " tiles with bits 4-5"
      << ", " << census.fishable_tiles << " fishable"
      << ", " << census.fatigue_tiles << " fatigue"
      << ", " << census.flow_vertices << " flow vertices"
      << ", " << census.flow_layers << " layers with flow data"
      << std::endl;
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

    liquid_fidelity fidelity_total;
    liquid_census census_original_total;
    liquid_census census_first_total;

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

        // read the first generation back while it is still on disk; the
        // liquid fidelity metrics compare the original against it
        adt_bytes first_saved;
        bool const first_read (read_file (first_saved_path, first_saved));

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

        // liquid fidelity metrics: informational, they do not fail the tile
        liquid_fidelity fidelity;

        if (first_read)
        {
          fidelity = compare_liquids (original, first_saved);

          liquid_census const census_original (census_liquids (original));
          liquid_census const census_first (census_liquids (first_saved));

          merge_census (census_original_total, census_original);
          merge_census (census_first_total, census_first);
          merge_fidelity (fidelity_total, fidelity);

          if (only_x >= 0)
          {
            std::string const tile_label ("tile " + std::to_string (x) + "-" + std::to_string (z));
            log_fidelity (tile_label + ", original vs first save", fidelity);
            log_census (tile_label + ", original", census_original);
            log_census (tile_label + ", first save", census_first);
          }
        }
        else
        {
          LogError << "adt_roundtrip: tile " << x << "-" << z
            << ": first saved generation is missing, liquid fidelity not measured" << std::endl;
        }

        if (chunks_ok && liquid_ok && stable)
        {
          ++tiles_ok;

          NOGGIT_LOG << "adt_roundtrip: tile " << x << "-" << z << " ok"
            << " (" << original.size() << " -> " << saved.size() << " bytes, "
            << original_stats.liquid_chunks << " liquid chunks preserved, "
            << original_stats.doodads << " m2 + " << original_stats.wmos << " wmo (models are not written in this mode))"
            << std::endl;

          if (first_read && fidelity.byte_identical != fidelity.chunks_compared)
          {
            LogError << "adt_roundtrip: tile " << x << "-" << z << ": "
              << (fidelity.chunks_compared - fidelity.byte_identical)
              << " of " << fidelity.chunks_compared
              << " liquid chunks are not byte-identical (dumps kept for inspection)" << std::endl;
          }
          else
          {
            std::error_code cleanup_error;
            std::filesystem::remove (saved_path, cleanup_error);
            std::filesystem::remove (first_saved_path, cleanup_error);
            std::filesystem::remove (original_dump_path, cleanup_error);
          }
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

    // informational; pass/fail above is decided on structure and stability
    log_fidelity ("original vs first save, all tiles", fidelity_total);
    log_census ("all tiles, original", census_original_total);
    log_census ("all tiles, first save", census_first_total);

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

    // the save path always writes into the configured project folder;
    // redirect it to the scratch folder while running
    QString const original_project_path (NoggitSettings.value ("project/path").toString());

    NoggitSettings.set_value ("project/path", QString::fromStdString (output_folder.string()) + "/");
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
    NoggitSettings.sync();

    NOGGIT_LOG << "adt_roundtrip: done with exit code " << result << std::endl;

    return result;
  }
}
