// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>
#include <math/ray.hpp>
#include <math/vector_3d.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/map_chunk_headers.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/Selection.h>
#include <noggit/tool_enums.hpp>
#include <util/sExtendableArray.hpp>

#include <vector>
#include <set>
#include <optional>

class MPQFile;
class MapChunk;

namespace noggit {
    namespace scripting {
        class chunk;
    }
}

class liquid_tile;

class liquid_chunk
{
public:
  liquid_chunk() = delete;
  explicit liquid_chunk(float x, float z, bool use_mclq_green_lava, liquid_tile* parent_tile);

  liquid_chunk (liquid_chunk const&) = delete;
  liquid_chunk (liquid_chunk&&) = delete;
  liquid_chunk& operator= (liquid_chunk const&) = delete;
  liquid_chunk& operator= (liquid_chunk&&) = delete;

  void from_mclq(std::vector<mclq>& layers);
  void fromFile(MPQFile &f, size_t basePos);
  void save(util::sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos);
  // writes the MCLQ chunk and returns the number of bytes written (chunk
  // header plus one mclq per stored layer); returns 0 and writes nothing when
  // the chunk has no liquid
  int save_mclq(util::sExtendableArray& adt, int mcnk_pos, int& current_pos);

  void copy_data(noggit::chunk_data& data) const;
  void override_data(noggit::chunk_data const& data, noggit::chunk_override_params const& params);
  void set_preview_data(noggit::chunk_data const& data, noggit::chunk_override_params const& params);
  void clear_preview();

  bool is_visible ( const float& cull_distance
                  , const math::frustum& frustum
                  , const math::vector_3d& camera
                  , display_mode display
                  ) const;

  void intersect(math::ray const& ray, selection_result* results);

  void autoGen(MapChunk* chunk, float factor);
  void auto_update_water_opacity(MapChunk* chunk);
  void update_underground_vertices_depth(MapChunk* chunk);
  void CropWater(MapChunk* chunkTerrain);

  void setType(int type, size_t layer);
  int getType(size_t layer) const;
  bool hasData(size_t layer) const;

  void paintLiquid( math::vector_3d const& pos
                  , float radius
                  , int liquid_id
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , math::vector_3d const& origin
                  , bool override_height
                  , bool override_liquid_id
                  , MapChunk* chunk
                  , float opacity_factor
                  );

  void clear_layers();

  float xbase, zbase;

  int displayed_layer_count() const { return displayed_layers().size(); }
  std::vector<liquid_layer>& displayed_layers() { return _preview_layers.empty() ? _layers : _preview_layers; }
  std::vector<liquid_layer>const& displayed_layers() const { return _preview_layers.empty() ? _layers : _preview_layers; }

  void upload_data(int& index_in_tile, liquid_render& render);
  void update_data(liquid_render& render);
  void update_indices_info(std::vector<void*>& indices_offsets, std::vector<int>& indices_count);
  void update_lod_level(math::vector_3d const& camera_pos, std::vector<void*>& indices_offsets, std::vector<int>& indices_count);

  math::vector_3d const& min() { return vmin; }
  math::vector_3d const& max() { return vmax; }

private:
  void update_attributes();

  std::vector<math::vector_3d> _intersect_points;

  math::vector_3d vmin, vmax, vcenter;
  bool _use_mclq_green_lava;

  // remove empty layers
  void cleanup();
  // update every layer's render
  void update_layers();

  void copy_height_to_layer(liquid_layer& target, math::vector_3d const& pos, float radius);

  MH2O_Attributes attributes;

  std::vector<liquid_layer> _layers;
  std::vector<liquid_layer> _preview_layers;
  int _layer_count = 0;

  liquid_tile* _liquid_tile;

  friend class noggit::scripting::chunk;
  friend class MapView;
};
