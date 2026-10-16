// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>
#include <math/trig.hpp>
#include <noggit/cursor_render.hpp>
#include <noggit/gizmo.hpp>
#include <noggit/map_chunk_headers.hpp>
#include <noggit/Misc.h>
#include <noggit/Model.h> // ModelManager
#include <noggit/Selection.h>
#include <noggit/Sky.h> // Skies, OutdoorLighting, OutdoorLightStats
#include <noggit/WMO.h> // WMOManager
#include <noggit/map_horizon.h>
#include <noggit/map_index.hpp>
#include <noggit/moveable_object.hpp>
#include <noggit/moveable_object_group.hpp>
#include <noggit/tile_index.hpp>
#include <noggit/texture_array_handler.hpp>
#include <noggit/tileset_array_handler.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/world_tile_update_queue.hpp>
#include <noggit/world_model_instances_storage.hpp>
#include <opengl/primitives.hpp>
#include <opengl/shader.fwd.hpp>
#include <opengl/texture.hpp>

#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace noggit
{
  struct object_paste_params;
  class chunk_mover;
}

class Brush;
class MapTile;

static const float detail_size = 8.0f;
static const float highresdistance = 384.0f;
static const float modeldrawdistance = 384.0f;
static const float doodaddrawdistance = 64.0f;

class World
{
private:
  int _model_display_mode = 0;

  std::vector<WMOInstance*> _wmos_with_skybox;
  std::unordered_map<std::string, std::vector<WMOInstance*>> _wmos_by_filename;
  std::unordered_map<std::string, std::vector<ModelInstance*>> _models_by_filename;
  std::unordered_map<std::string, std::vector<ModelInstance*>> _wmo_doodads_by_filename;
  std::unordered_map<std::string, std::vector<ModelInstance*>> _models_by_filename_with_wmo_doodads;

  std::vector<std::pair<wmo_liquid*, math::matrix_4x4>> _wmo_liquids_to_draw;

  noggit::world_model_instances_storage _model_instance_storage;
  noggit::world_tile_update_queue _tile_update_queue;

  noggit::tileset_array_handler _tileset_handler;
  noggit::texture_array_handler _model_texture_handler;

  std::string _last_selected_texture = "";

public:
  int model_instance_count() const { return _model_instance_storage.size(); }

  MapIndex mapIndex;
  noggit::map_horizon horizon;

  // Temporary variables for loading a WMO, if we have a global WMO.
  std::string mWmoFilename;
  ENTRY_MODF mWmoEntry;

  // Vertex Buffer Objects for coordinates used for drawing.
  GLuint detailtexcoords;

  // The lighting used.
  std::unique_ptr<OutdoorLighting> ol;

  unsigned int getMapID();
  // Time of the day.
  float animtime;
  float time;

  //! \brief Name of this map.
  std::string basename;

  // Dynamic distances for rendering. Actually, these should be the same..
  float fogdistance;
  float culldistance;

  std::unique_ptr<Skies> skies;

  OutdoorLightStats outdoorLightStats;

  explicit World(const std::string& name, int map_id);
  ~World();

  void initDisplay();

  void update_models_emitters(float dt);
  void draw ( math::matrix_4x4 const& model_view
            , math::matrix_4x4 const& projection
            , math::frustum const& frustum
            , math::vector_3d const& cursor_pos
            , math::vector_4d const& cursor_color
            , int cursor_type
            , bool square_brush
            , float brush_radius
            , bool show_liquid_cursor
            , bool show_unpaintable_chunks
            , std::string const& current_texture
            , bool draw_contour
            , float inner_radius_ratio
            , math::vector_3d const& ref_pos
            , float angle
            , float orientation
            , bool use_ref_pos
            , bool angled_mode
            , bool draw_chunk_flag_overlay
            , bool draw_areaid_overlay
            //! \todo passing editing_mode is _so_ wrong, I don't believe I'm doing this
            , editing_mode
            , math::vector_3d const& camera_pos
            , bool camera_moved
            , bool draw_mfbo
            , bool draw_wireframe
            , bool draw_lines
            , bool draw_terrain
            , bool draw_wmo
            , bool draw_water
            , bool draw_doodads
            , bool draw_models
            , bool draw_model_animations
            , bool draw_hole_lines
            , bool draw_models_with_box
            , bool draw_hidden_models
            , bool draw_sky
            , bool draw_skybox
            , bool draw_shadows
            , bool draw_vertex_colors
            , bool use_dbc_lighting_data
            , std::map<int, misc::random_color>& area_id_colors
            , bool draw_fog
            , eTerrainType ground_editing_brush
            , int water_layer
            , display_mode display
            );

  void update_legacy_shadow_for_tile_at(math::vector_3d const& pos, float pitch, int threshold);

private:
  void update_legacy_shadows(MapTile* adt, float pitch, int threshold);

public:

  unsigned int getAreaID (math::vector_3d const&);
  void setAreaID(math::vector_3d const& pos, int id, bool adt);

  selection_result intersect ( math::matrix_4x4 const& model_view
                             , math::ray const&
                             , bool only_map
                             , bool do_objects
                             , bool draw_terrain
                             , bool draw_wmo
                             , bool draw_models
                             , bool draw_hidden_models
                             , bool intersect_liquids
                             , bool ignore_terrain_holes = true
                             );

  void initGlobalVBOs(GLuint* pDetailTexCoords);

private:
  // Information about the currently selected model / WMO / triangle.
  std::vector<selection_type> _current_selection;
  noggit::moveable_object_group _grouped_models;
  int _selected_model_count = 0;
  bool _only_one_model_box;
public:

  noggit::gizmo gizmo;
  std::optional<math::vector_3d> multi_select_pivot() const { return _grouped_models.pivot(); }

  // Selection related methods.
  bool is_selected(selection_type selection) const;
  bool is_selected(std::uint32_t uid) const;
  std::vector<selection_type> const& current_selection() const { return _current_selection; }
  std::optional<selection_type> get_last_selected_model() const;
  bool has_selection() const { return !_current_selection.empty(); }
  bool has_multiple_model_selected() const { return _selected_model_count > 1; }
  void set_current_selection(selection_type entry);
  void add_to_selection(selection_type entry);
  void remove_from_selection(selection_type entry);
  void remove_from_selection(std::uint32_t uid);
  void reset_selection();
  void delete_selected_models();
  void reset_selection_xz_rotation() { _grouped_models.reset_xz_rotation(this); }

  void raise_models_terrain_brush(math::vector_3d const& pos, float change, float radius, int BrushType, float inner_radius, bool follow_normals);

  enum class m2_scaling_type
  {
    set,
    add,
    mult
  };

  void snap_selected_models_to_the_ground();
  void scale_selected_models(float v, m2_scaling_type type);
  void move_selected_models(float dx, float dy, float dz);
  void move_selected_models(math::vector_3d const& delta)
  {
    move_selected_models(delta.x, delta.y, delta.z);
  }
  void set_selected_models_pos(float x, float y, float z, bool change_height = true)
  {
    return set_selected_models_pos({x,y,z}, change_height);
  }
  void set_selected_models_pos(math::vector_3d const& pos, bool change_height = true);
  void rotate_selected_models(math::degrees rx, math::degrees ry, math::degrees rz, bool use_pivot);
  void rotate_selected_models_randomly(float minX, float maxX, float minY, float maxY, float minZ, float maxZ);
  void set_selected_models_rotation(math::degrees rx, math::degrees ry, math::degrees rz);
  // Checks the normal of the terrain on model origin and rotates to that spot.
  void rotate_selected_models_to_ground_normal(bool smoothNormals);


  std::optional<math::degrees::vec3> get_terrain_normal(math::vector_3d const& pos, bool smooth_normal);
  bool GetVertex(float x, float z, math::vector_3d *V) const;
  std::optional<float> get_exact_height_at(math::vector_3d const& pos);

  // check if the cursor is under map or in an unloaded tile
  bool isUnderMap(math::vector_3d const& pos);

  template<typename Fun>
    bool for_all_chunks_in_range ( math::vector_3d const& pos
                                 , float radius
                                 , Fun&& /* MapChunk* -> bool changed */
                                 );
  template<typename Fun, typename Post>
    bool for_all_chunks_in_range ( math::vector_3d const& pos
                                 , float radius
                                 , Fun&& /* MapChunk* -> bool changed */
                                 , Post&& /* MapChunk* -> void; called for all changed chunks */
                                 );
  template<typename Fun>
    void for_all_chunks_on_tile (math::vector_3d const& pos, Fun&&);

  template<typename Fun>
    void for_chunk_at(math::vector_3d const& pos, Fun&& fun);
  template<typename Fun>
    auto for_maybe_chunk_at (math::vector_3d const& pos, Fun&& fun) -> std::optional<decltype (fun (nullptr))>;

  template<typename Fun>
    void for_tile_at(const tile_index& pos, Fun&&);
  template<typename Fun>
    bool for_all_tiles_in_range ( math::vector_3d const& pos
                                , float radius
                                , Fun&& /* MapTile* -> bool changed */
                                );

  MapChunk * get_chunk_at(math::vector_3d const& pos);

  void load_full_map();

  void changeTerrain(math::vector_3d const& pos, float change, float radius, int BrushType, float inner_radius, terrain_edit_mode edit_mode);
  void changeShader(math::vector_3d const& pos, math::vector_4d const& color, float change, float radius, bool editMode);
  math::vector_3d pickShaderColor(math::vector_3d const& pos);
  void flattenTerrain(math::vector_3d const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const math::vector_3d& origin, math::degrees angle, math::degrees orientation);
  void blurTerrain(math::vector_3d const& pos, float remain, float radius, int BrushType, flatten_mode const& mode);
  bool paintTexture(math::vector_3d const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture);
  bool sprayTexture(math::vector_3d const& pos, Brush *brush, float strength, float pressure, float spraySize, float sprayPressure, scoped_blp_texture_reference texture);
  bool replaceTexture(math::vector_3d const& pos, Brush const& brush, float change, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture);

  void clear_on_chunks ( math::vector_3d const& pos, float radius, bool height, bool textures, bool duplicate_textures
                       , bool textures_below_threshold, float alpha_threshold, bool texture_flags, bool liquids
                       , bool m2s, bool wmos, bool shadows, bool mccv, bool impassible_flag, bool holes
                       );
  void clear_on_tiles ( math::vector_3d const& pos, float radius, bool height, bool textures, bool duplicate_textures
                      , bool textures_below_threshold, float alpha_threshold, bool texture_flags, bool liquids
                      , bool m2s, bool wmos, bool shadows, bool mccv, bool impassible_flag, bool holes
                      );

  void eraseTextures(math::vector_3d const& pos);
  void overwriteTextureAtCurrentChunk(math::vector_3d const& pos, scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture);
  void setBaseTexture(math::vector_3d const& pos);
  void clear_shadows(math::vector_3d const& pos);
  void clearTextures(math::vector_3d const& pos);
  void swapTexture(math::vector_3d const& pos, scoped_blp_texture_reference tex);
  void removeTexDuplicateOnADT(math::vector_3d const& pos);
  void change_texture_flag(math::vector_3d const& pos, scoped_blp_texture_reference const& tex, std::size_t flag, bool add);

  void setHole(math::vector_3d const& pos, bool big, bool hole);
  void setHoleADT(math::vector_3d const& pos, bool hole);

  void set_shadow(math::vector_3d const& pos, float radius, bool add);

  ModelInstance* addM2 ( std::string const& filename
             , math::vector_3d newPos
             , float scale, math::degrees::vec3 rotation
             , noggit::object_paste_params*
             );
  WMOInstance* addWMO ( std::string const& filename
              , math::vector_3d newPos
              , math::degrees::vec3 rotation
              );

  void add_model(noggit::model_placement_data const& data);

  // add a m2 instance to the world (needs to be positioned already), return the uid
  std::uint32_t add_model_instance(ModelInstance model_instance, bool from_reloading);
  // add a wmo instance to the world (needs to be positioned already), return the uid
  std::uint32_t add_wmo_instance(WMOInstance wmo_instance, bool from_reloading);

  std::optional<selection_type> get_model(std::uint32_t uid);
  void remove_models_if_needed(std::vector<uint32_t> const& uids);
  void remove_models_on_chunk(math::vector_3d const& chunk_origin)
  {
    _model_instance_storage.delete_instances_on_chunk(chunk_origin);
    need_model_updates = true;
  }

  std::vector<selection_type> get_models_on_chunk(math::vector_3d const& chunk_origin) { return _model_instance_storage.get_instances_on_chunk(chunk_origin); }

  void reload_tile(tile_index const& tile);
  void ensure_tile_is_loaded(tile_index const& tile);

  void updateTilesEntry(selection_type const& entry, model_update type);
  void updateTilesWMO(WMOInstance* wmo, model_update type);
  void updateTilesModel(ModelInstance* m2, model_update type);
  void wait_for_all_tile_updates();

  void saveMap (int width, int height);

  void deleteModelInstance(int pUniqueID);
  void deleteWMOInstance(int pUniqueID);

  bool uid_duplicates_found() const;
  void delete_duplicate_model_and_wmo_instances();
  // used after the uid fix all
  void unload_every_model_and_wmo_instance();

	static bool IsEditableWorld(int pMapId);

  void clearHeight(math::vector_3d const& pos);
  void clearAllModelsOnADT(tile_index const& tile);

  // liquids
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
                  , float opacity_factor
                  );
  void CropWaterADT(const tile_index& pos);
  void setWaterType(const tile_index& pos, int type, int layer);
  int getWaterType(const tile_index& tile, int layer);
  void autoGenWaterTrans(const tile_index&, float factor);
  void update_water_opacity(math::vector_3d const& pos, float radius);

  void fixAllGaps();

  void convert_alphamap(bool to_big_alpha);

  bool deselectVertices(math::vector_3d const& pos, float radius);
  void selectVertices(math::vector_3d const& pos, float radius);
  void delete_models(std::vector<selection_type> const& types);
  void selectVertices(math::vector_3d const& pos1, math::vector_3d const& pos2);
  std::set<math::vector_3d*>* getSelectedVertices();

  template<typename Fun>
  bool for_all_chunks_between ( math::vector_3d const& pos1,
                                math::vector_3d const& pos2,
                                Fun&& /* MapChunk* -> bool changed */
                                );

  void select_all_chunks_between(math::vector_3d const& pos1, math::vector_3d const& pos2, std::vector<MapChunk*>& chunks_in);

  template<typename Fun>
  void for_each_wmo_instance(Fun&& function)
  {
    _model_instance_storage.for_each_wmo_instance(function);
  }

  template<typename Fun>
  void for_each_m2_instance(Fun&& function)
  {
    _model_instance_storage.for_each_m2_instance(function);
  }

  void moveVertices(float h);
  void orientVertices ( math::vector_3d const& ref_pos
                      , math::degrees vertex_angle
                      , math::degrees vertex_orientation
                      );
  void flattenVertices (float height);

  void updateSelectedVertices();
  void updateVertexCenter();
  void clearVertexSelection();

  math::vector_3d const& vertexCenter();

  void recalc_norms (MapChunk*) const;

  bool need_model_updates = false;

  void select_chunks_in_range(math::vector_3d const& pos, float radius, bool square_select, bool deselect, noggit::chunk_mover& chunk_mover);

private:
  void clear_on_chunk( MapChunk* chunk, bool height, bool textures, bool duplicate_textures
                     , bool textures_below_threshold, float alpha_threshold, bool texture_flags, bool liquids
                     , bool shadows, bool mccv, bool impassible_flag, bool holes
                     );


  void update_models_by_filename();

  bool _need_wmo_liquid_update = true;

  // set in ~World so tile destructors stop touching half-destroyed members
  bool _tearing_down = false;

  bool _models_still_loading = true;
  int _last_unloaded_doodad_check = 0;

  std::set<MapChunk*>& vertexBorderChunks();

  std::set<MapTile*> _vertex_tiles;
  std::set<MapChunk*> _vertex_chunks;
  std::set<MapChunk*> _vertex_border_chunks;
  std::set<math::vector_3d*> _vertices_selected;
  math::vector_3d _vertex_center;
  bool _vertex_center_updated = false;
  bool _vertex_border_updated = false;

  std::unique_ptr<noggit::map_horizon::render> _horizon_render;

  bool _display_initialized = false;

  float _view_distance;

  std::unique_ptr<opengl::program> _gizmo_program;
  std::unique_ptr<opengl::program> _mcnk_program;
  std::unique_ptr<opengl::program> _mfbo_program;
  std::unique_ptr<opengl::program> _m2_program;
  std::unique_ptr<opengl::program> _m2_instanced_program;
  std::unique_ptr<opengl::program> _m2_particles_program;
  std::unique_ptr<opengl::program> _m2_ribbons_program;
  std::unique_ptr<opengl::program> _m2_box_program;
  std::unique_ptr<opengl::program> _wmo_program;

  std::unique_ptr<opengl::program> _wmo_depth_program;
  std::unique_ptr<opengl::program> _m2_depth_program;
  std::unique_ptr<opengl::program> _shadow_program;

  noggit::cursor_render _cursor_render;
  opengl::primitives::sphere _sphere_render;
  opengl::primitives::square _square_render;

  std::optional<liquid_render> _liquid_render = std::nullopt;

  opengl::scoped::deferred_upload_framebuffers<2> _framebuffers;
  GLuint const& _depth_framebuffer = _framebuffers[0];
  GLuint const& _shadow_framebuffer = _framebuffers[1];

  opengl::texture _depth_texture;
  opengl::texture _shadow_texture;
};
