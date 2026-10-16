// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/ray.hpp>
#include <noggit/map_enums.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/Selection.h>
#include <noggit/liquid_tile.hpp>
#include <noggit/tile_index.hpp>
#include <noggit/tileset_array_handler.hpp>
#include <noggit/tool_enums.hpp>
#include <opengl/shader.fwd.hpp>
#include <noggit/Misc.h>

#include <map>
#include <string>
#include <vector>

namespace math
{
  class frustum;
  struct vector_3d;
}

class World;

class MapTile : public AsyncObject
{
public:
	MapTile( int x0
         , int z0
         , std::string const& pFilename
         , bool pBigAlpha
         , bool pLoadModels
         , bool use_mclq_green_lava
         , bool reloading_tile
         , World*
         , tile_mode mode = tile_mode::edit
         );
  ~MapTile();

  void finishLoading();

  void convert_alphamap(bool to_big_alpha);

  //! \brief Get chunk for sub offset x,z.
  MapChunk* getChunk(unsigned int x, unsigned int z);
  //! \todo map_index style iterators
  std::vector<MapChunk*> chunks_in_range (math::vector_3d const& pos, float radius) const;
  //! \note inclusive, i.e. getting both ADTs if point is on a border
  std::vector<MapChunk*> chunks_between (math::vector_3d const& pos1, math::vector_3d const& pos2) const;

  const tile_index index;
  float xbase, zbase;

  std::atomic<bool> changed;

  void draw ( math::frustum const& frustum
            , opengl::scoped::use_program& mcnk_shader
            , GLuint const& tex_coord_vbo
            , const float& cull_distance
            , const math::vector_3d& camera
            , bool need_visibility_update
            , bool selected_texture_changed
            , std::string const& current_texture
            , std::map<int, misc::random_color>& area_id_colors
            , display_mode display
            , noggit::tileset_array_handler& tileset_handler
            );
  void draw_shadows(opengl::scoped::use_program& shadow_shader);
  void intersect (math::ray const& ray, selection_result* results, bool ignore_terrain_holes);
  void intersect_liquids (math::ray const&, selection_result*);


  void drawWater ( math::frustum const& frustum
                 , const float& cull_distance
                 , const math::vector_3d& camera
                 , bool camera_moved
                 , liquid_render& render
                 , opengl::scoped::use_program& water_shader
                 , int animtime
                 , int layer
                 , display_mode display
                 );

  void drawMFBO (opengl::scoped::use_program&);

  bool GetVertex(float x, float z, math::vector_3d *V);

	void CropWater();

  void set_shadows(std::vector<std::uint8_t> const& shadow_map, int threshold);

  void saveTile(World* world);

private:
  void save(World* world);

public:

  bool isTile(int pX, int pZ);

  virtual async_priority loading_priority() const
  {
    return async_priority::high;
  }

  bool has_model(uint32_t uid) const
  {
    return std::find(uids.begin(), uids.end(), uid) != uids.end();
  }

  void remove_model(uint32_t uid);
  void add_model(uint32_t uid);

  liquid_tile Water;

  bool tile_is_being_reloaded() const { return _tile_is_being_reloaded; }
  bool use_no_alpha_alphamap() const { return _use_no_alpha_alphamap; }
  void require_regular_alphamap();
private:
  opengl::texture_array _adt_alphamap;
  bool _use_no_alpha_alphamap = false;
  bool _alphamap_created = false;
  void create_combined_alpha_shadow_map();

  void upload();
  bool _uploaded = false;
public:
  // todo: store extent for each part and only update the relevant one and then update the "global" extents
  void water_height_changed() { _need_recalc_extents = true; }
  void chunk_height_changed() { _need_recalc_extents = true; _need_visibility_update = true; _need_chunk_data_update = true; }
  void need_chunk_data_update() { _need_chunk_data_update = true; }

  bool is_visible() const { return _is_visible; }

  std::array<math::vector_3d, 2> const& extents() const { return _extents; }
  std::vector<math::vector_3d> const& intersect_points() const { return _intersect_points; }
private:
  bool _need_chunk_data_update = true;

  std::array<math::vector_3d, 2> _extents;
  std::vector<math::vector_3d> _intersect_points;

  bool _need_recalc_extents = true;
  void recalc_extents();

  bool _need_visibility_update = true;
  bool _is_visible = false;

  void update_visibility( const float& cull_distance
                        , const math::frustum& frustum
                        , const math::vector_3d& camera
                        , display_mode display
                        );

  tile_mode _mode;
  bool _tile_is_being_reloaded;

  // MFBO:
  math::vector_3d mMinimumValues[3 * 3];
  math::vector_3d mMaximumValues[3 * 3];

  bool _mfbo_buffer_are_setup = false;
  opengl::scoped::deferred_upload_vertex_arrays<2> _mfbo_vaos;
  GLuint const& _mfbo_bottom_vao = _mfbo_vaos[0];
  GLuint const& _mfbo_top_vao = _mfbo_vaos[1];
  opengl::scoped::deferred_upload_buffers<2> _mfbo_vbos;
  GLuint const& _mfbo_bottom_vbo = _mfbo_vbos[0];
  GLuint const& _mfbo_top_vbo = _mfbo_vbos[1];

  opengl::scoped::deferred_upload_buffers<1> _ubo;
  GLuint const& _chunks_data_ubo = _ubo[0];

  opengl::scoped::deferred_upload_vertex_arrays<2> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  GLuint const& _shadow_vao = _vertex_array[1];
  opengl::scoped::deferred_upload_buffers<2> _vertex_buffers;
  GLuint const& _vertices_vbo = _vertex_buffers[0];
  GLuint const& _indices_vbo = _vertex_buffers[1];

  std::vector<void*> _indices_offsets;
  std::vector<int> _indices_count;

  // MHDR:
  int mFlags;
  bool mBigAlpha;

  // Data to be loaded and later unloaded.
  std::vector<std::string> mTextureFilenames;
  std::vector<std::string> mModelFilenames;
  std::vector<std::string> mWMOFilenames;

  std::map<std::string, mtxf_entry> _mtxf_entries;

  std::vector<uint32_t> uids;

  std::unique_ptr<MapChunk> mChunks[16][16];

  bool _load_models;
  World* _world;

  friend class MapChunk;
  friend class TextureSet;
};
