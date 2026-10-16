// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/trig.hpp>
#include <math/ray.hpp>
#include <noggit/liquid_render.hpp>
#include <noggit/map_chunk_headers.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/Selection.h>
#include <math/vector_2d.hpp>
#include <util/sExtendableArray.hpp>

class MapChunk;

using liquid_indice = std::uint16_t;

// handle liquids like oceans, lakes, rivers, slime, magma
class liquid_layer
{
public:
  liquid_layer() = delete;
  liquid_layer(math::vector_3d const& base, float height, int liquid_id);
  liquid_layer(math::vector_3d const& base, mclq& liquid, int liquid_id);
  liquid_layer(MPQFile &f, std::size_t base_pos, math::vector_3d const& base, MH2O_Information const& info, std::uint64_t infomask);
  liquid_layer(math::vector_3d const& base, noggit::liquid_layer_data const& data);

  liquid_layer(liquid_layer const& other);
  liquid_layer (liquid_layer&&);

  liquid_layer& operator=(liquid_layer&&);
  liquid_layer& operator=(liquid_layer const& other);

  void save(util::sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const;
  mclq to_mclq(MH2O_Attributes& attributes) const;

  void copy_data(noggit::chunk_data& data) const;

  void update_attributes(MH2O_Attributes& attributes);
  void update_indices();
  void changeLiquidID(int id);

  void crop(MapChunk* chunk);
  void update_opacity(MapChunk* chunk, float factor);
  void update_underground_vertices_depth(MapChunk* chunk);

  void intersect(math::ray const& ray, selection_result* results);


  float min() const { return _minimum; }
  float max() const { return _maximum; }
  int liquidID() const { return _liquid_id; }
  int mclq_liquid_type() const { return _mclq_liquid_type; }
  // order of the flag corresponding to the liquid type in the mcnk header
  int mclq_flag_ordering() const;

  // used for fatigue calculation
  bool subchunk_at_max_depth(int x, int z) const;

  bool hasSubchunk(int x, int z, int size = 1) const;
  void setSubchunk(int x, int z, bool water);

  bool empty() const { return !_subchunks; }
  bool full() const { return _subchunks == std::uint64_t(-1); }
  void clear() { _subchunks = std::uint64_t(0); }

  void paintLiquid( math::vector_3d const& pos
                  , float radius
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , math::vector_3d const& origin
                  , bool override_height
                  , MapChunk* chunk
                  , float opacity_factor
                  );

  void copy_subchunk_height(int x, int z, liquid_layer const& from);

  static constexpr int lod_count = 3;
  static constexpr int vertex_buffer_size_required = 9 * 9 * sizeof(liquid_vertex);
  static constexpr std::array<int, 4> max_indices_per_lod_level =
    {{ 8 * 8 * 2 * 3  // 2 triangles per quad
     , 4 * 4 * 2 * 3
     , 2 * 2 * 2 * 3
     , 1 * 1 * 2 * 3
    }};
  static constexpr std::array<int, 4> lod_level_offset =
  {{ (0              ) * 6 * sizeof(liquid_indice)
   , (0 + 64         ) * 6 * sizeof(liquid_indice)
   , (0 + 64 + 16    ) * 6 * sizeof(liquid_indice)
   , (0 + 64 + 16 + 4) * 6 * sizeof(liquid_indice)
  }};
  static constexpr int max_total_indices = max_indices_per_lod_level[0] + max_indices_per_lod_level[1]
                                         + max_indices_per_lod_level[2];
  static constexpr int indice_buffer_size_required = max_total_indices * sizeof(liquid_indice);


  void upload_data(int index_in_tile, liquid_render& render)
  {
    _index_in_tile = index_in_tile;
    update_data(render);
  }

  void update_data(liquid_render& render);
  void update_indices_info(std::vector<void*>& indices_offsets, std::vector<int>& indices_count);

  int get_lod_level(math::vector_3d const& camera_pos) const;
  void set_lod_level(int lod_level, std::vector<void*>& indices_offsets, std::vector<int>& indices_count);

  int liquid_id() const { return _liquid_id; }
  int liquid_type() const { return _liquid_type; }
  bool has_fatigue() const { return _fatigue_enabled; }
private:
  bool check_fatigue() const;
  bool _fatigue_enabled = false;

  // used to get the offset in the tile's buffers for the layer
  int _index_in_tile;
  bool _need_data_update = true;

private:
  void create_vertices(float height);

  void update_min_max();
  void update_vertex_opacity(int x, int z, MapChunk* chunk, float factor);


  int _current_lod_level = 0;
  int _current_lod_indices_count = 0;


  int _liquid_id;
  int _liquid_type;
  int _liquid_vertex_format;
  int _mclq_liquid_type;
  float _minimum;
  float _maximum;
  math::vector_3d _center;

  std::uint64_t _subchunks;

  // raw mclq fields that have no editor-side representation, kept so an
  // untouched chunk is saved back byte-identically: vanilla data uses per
  // vertex flow, per tile attribute bits and the flow volume list, all of
  // which would otherwise be zeroed on save
  bool _from_mclq = false;
  std::uint64_t _mclq_covered = 0;      // tiles that had liquid in the file
  std::uint64_t _mclq_fishable = 0;
  std::uint64_t _mclq_fatigue = 0;
  std::uint64_t _mclq_flag_0x10 = 0;
  std::uint64_t _mclq_flag_0x20 = 0;
  std::array<std::uint8_t, 9 * 9> _mclq_flow_0 = {};
  std::array<std::uint8_t, 9 * 9> _mclq_flow_1 = {};
  std::array<std::uint8_t, 9 * 9> _mclq_depth = {};
  // vertices whose depth the tile loader zeroed for rendering (lods need
  // below-terrain vertices to be transparent): an untouched vertex must
  // save back its raw byte, not the render hack's zero
  std::array<std::uint8_t, 9 * 9> _render_zeroed_depth = {};
  std::uint32_t _mclq_n_flowvs = 0;
  std::array<mclq_flowvs, 2> _mclq_flowvs = {};

  std::vector<liquid_vertex> _vertices;

  std::map<int, std::vector<liquid_indice>> _indices_by_lod;
  std::map<int, int> _indices_count_by_lod;

  math::vector_3d pos;
};
