// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/map_chunk_headers.hpp>

#include <math/quaternion.hpp> // math::vector_4d
#include <noggit/Misc.h>
#include <noggit/ModelInstance.h>
#include <noggit/Selection.h>
#include <noggit/TextureManager.h>
#include <noggit/WMOInstance.h>
#include <noggit/map_enums.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/tileset_array_handler.hpp>
#include <noggit/tool_enums.hpp>
#include <opengl/scoped.hpp>
#include <opengl/texture.hpp>
#include <util/sExtendableArray.hpp>

#include <map>
#include <memory>
#include <optional>

class MPQFile;
namespace math
{
  class frustum;
  struct vector_4d;
}
class Brush;
class liquid_chunk;
class MapTile;


namespace noggit
{
  class chunk_data;
}

class MapChunk
{
public:

  static constexpr int max_indices_count_without_lod = 8 * 8 * 4 * 3; // 8x8 squares with 4 triangls
  static constexpr int lod_count = 4; // max 4, last lod level is a single quad for the chunk (which doesn't look good)
  static constexpr int indice_buffer_count = lod_count + 1;
  static constexpr std::array<int, 5> max_indices_per_lod_level = {{768, 384, 96, 24, 6}};

  static constexpr int total_indices_count_with_lods()
  {
    int count = 0;

    for (int i = 0; i < indice_buffer_count; ++i)
    {
      count += max_indices_per_lod_level[i];
    }

    return count;
  }

private:
  static std::vector<chunk_indice> strip_without_holes; // it's always the same, no need to recreate it each time

  chunk_shader_data _shader_data;
  tile_mode _mode;

  bool _has_mccv;
  std::uint32_t _4x4_holes;
  unsigned int _area_id;

  std::unique_ptr<chunk_shadow> _chunk_shadow;

  std::map<int, std::vector<chunk_indice>> _indice_strips;
  std::map<int, int> _indices_count_per_lod_level;

  bool shadow_map_is_empty() const;

  int indices_count(int lod_level) const;
  void initStrip();

  int indexNoLoD(int z, int x);
  int indexLoD(int z, int x);

  std::vector<math::vector_3d> _intersect_points;

  void update_intersect_points();

  int get_lod_level(math::vector_3d const& camera_pos, display_mode display) const;

  bool _uploaded = false;
  bool _need_indice_buffer_update = true;
  bool _need_lod_update = true;
  bool _need_vao_update = true;

  bool _is_copied = false;
  bool _is_in_paste_zone = false;

  std::unique_ptr<noggit::chunk_data> _preview_data;
  std::unique_ptr<noggit::chunk_override_params> _preview_params;
public:
  MapChunk(MapTile* mt, MPQFile* f, bool bigAlpha, tile_mode mode);
  noggit::chunk_data get_chunk_data();
  void override_data(noggit::chunk_data& data, noggit::chunk_override_params const& params);
  void set_preview_data(noggit::chunk_data& data, noggit::chunk_override_params const& params);
  void set_copied(bool v);
  void set_is_in_paste_zone(bool v);

  MapTile *mt;
  math::vector_3d vmin, vmax, vcenter;
  int px, py;

  int chunk_index() const { return px + 16 * py; }
  int vertex_offset() const { return chunk_index() * mapbufsize; }
  int indices_offset() const { return chunk_index() * total_indices_count_with_lods(); }
  void* lod_indices_ptr(int lod) const;

  MapChunkHeader header;

  float xbase, ybase, zbase;

  bool use_big_alphamap;

  std::unique_ptr<TextureSet> texture_set;

  std::array<chunk_vertex, mapbufsize> vertices;

  bool is_visible ( const float& cull_distance
                  , const math::frustum& frustum
                  , const math::vector_3d& camera
                  , display_mode display
                  ) const;

  int current_lod_indices_count() const { return _indices_count_per_lod_level.at(_lod_level); }

  bool is_currently_visible() const { return _is_visible; }

  void set_visible();
  void update_visibility ( const float& cull_distance
                         , const math::frustum& frustum
                         , const math::vector_3d& camera
                         , display_mode display
                         );
private:
  bool _is_visible = true; // visible by default
  bool _need_visibility_update = true;
  int _lod_level = 0;

  bool _shader_data_need_update = true;
  bool _texture_set_need_update = true;
public:
  void require_vertices_buffer_update() { _need_vao_update = true; }
  void require_shader_data_update();
  void texture_set_changed();

  void update_shader_data ( bool selected_texture_changed
                          , std::string const& current_texture
                          , std::map<int, misc::random_color>& area_id_colors
                          , noggit::tileset_array_handler& tileset_handler
                          , bool force_update = false
                          );

  void prepare_draw( const math::vector_3d& camera
                   , bool need_visibility_update
                   , bool selected_texture_changed
                   , std::string const& current_texture
                   , std::map<int, misc::random_color>& area_id_colors
                   , display_mode display
                   , noggit::tileset_array_handler& tileset_handler
                   , std::vector<void*>& indices_offsets
                   , std::vector<int>& indices_count
                   );

  void intersect (math::ray const& ray, selection_result* results, bool ignore_terrain_holes);
  bool ChangeMCCV(math::vector_3d const& pos, math::vector_4d const& color, float change, float radius, bool editMode);
  //! Initialize MCCV to 1,1,1, do nothing if already exists.
  void maybe_create_mccv();
  void reset_mccv();
  bool hasColors();
  math::vector_3d pickMCCV(math::vector_3d const& pos);

  ::liquid_chunk* liquid_chunk() const;

  void updateVerticesData();
  void recalcNorms (std::function<std::optional<float> (float, float)> height);

  //! \todo implement Action stack for these
  bool changeTerrain(math::vector_3d const& pos, float change, float radius, int BrushType, float inner_radius, terrain_edit_mode edit_mode);
  bool flattenTerrain(math::vector_3d const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const math::vector_3d& origin, math::degrees angle, math::degrees orientation);
  bool blurTerrain ( math::vector_3d const& pos, float remain, float radius, int BrushType, flatten_mode const& mode
                   , std::function<std::optional<float> (float, float)> height
                   );

  bool smooth_inner_vertices(math::vector_3d const& pos, float remain, float radius);

  void selectVertex(math::vector_3d const& pos, float radius, std::set<math::vector_3d*>& selected_vertices);
  void fixVertices(std::set<math::vector_3d*>& selected);
  // for the vertex tool
  bool isBorderChunk(std::set<math::vector_3d*>& selected);

  //! \todo implement Action stack for these
  bool paintTexture(math::vector_3d const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture);
  bool replaceTexture(math::vector_3d const& pos, Brush const& brush, float change, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture);
  bool canPaintTexture(std::string const& texture);
  int addTexture(scoped_blp_texture_reference texture);
  void switchTexture(scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture);
  void eraseTextures();
  void remove_texture_duplicates();
  void remove_unused_textures(float threshold);
  void change_texture_flag(scoped_blp_texture_reference const& tex, std::size_t flag, bool add);
  void clear_texture_flags();

  void clear_shadows();
  void set_shadow(math::vector_3d const& pos, float radius, bool add);
  void set_shadows(std::vector<std::uint8_t> const& tile_shadow_map, int threshold);

  //! \todo implement Action stack for these
  bool isHole(int i, int j) const;
  void setHole(math::vector_3d const& pos, bool big, bool add);

  void setFlag(bool value, uint32_t);

  int getAreaID();
  void setAreaID(int ID);

  bool GetVertex(float x, float z, math::vector_3d *V);
  float getHeight(int x, int z);
  float getMinHeight() const { return vmin.y; }
  std::optional<float> get_exact_height_at(math::vector_3d const& pos);

  void clearHeight();

  //! \todo this is ugly create a build struct or sth
  void save( util::sExtendableArray &lADTFile
           , int &lCurrentPosition
           , int &lMCIN_Position
           , std::map<std::string, int> &lTextures
           , std::vector<WMOInstance> &lObjectInstances
           , std::vector<ModelInstance>& lModelInstances
           );

  // fix the gaps with the chunk to the left
  bool fixGapLeft(const MapChunk* chunk);
  // fix the gaps with the chunk above
  bool fixGapAbove(const MapChunk* chunk);

  void selectVertex(math::vector_3d const& minPos, math::vector_3d const& maxPos, std::set<math::vector_3d*>& selected_vertices);

  void update_alpha_shadow_map();
};
