// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/quaternion.hpp>
#include <math/ray.hpp>
#include <math/vector_3d.hpp>
#include <noggit/MPQ.h>
#include <noggit/ModelInstance.h> // ModelInstance
#include <noggit/ModelManager.h>
#include <noggit/multimap_with_normalized_key.hpp>
#include <noggit/TextureManager.h>
#include <noggit/texture_array_handler.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/wmo_liquid.hpp>
#include <noggit/wmo_headers.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class WMO;
class WMOGroup;
class WMOInstance;
class WMOManager;
class wmo_liquid;
class Model;

struct wmo_group_uniform_data
{
  int cull = -1;
  int blend_mode = -1;
};

struct wmo_render_batch
{
  int blend_mode = 0;
  int index_start;
  int index_count;
  bool cull;
};

struct wmo_ubo_data
{
  std::uint64_t texture_1;
  std::uint64_t padding_1;
  std::uint64_t texture_2;
  std::uint64_t padding_2;

  int index_1;
  int index_2;
  int use_vertex_color;
  int exterior_lit;

  int shader_id;
  int unfogged;
  int unlit;
  float alpha_test;
};

class WMOGroup
{
public:
  WMOGroup(WMO *wmo, MPQFile* f, int num, char const* names, size_t names_size);
  WMOGroup(WMOGroup const&);
  ~WMOGroup();

  void load();

  void setup_global_buffer_data(std::vector<wmo_vertex>& vertices, std::vector<std::uint32_t>& indices);
  void setup_ubo_data();

  void draw( opengl::scoped::use_program& wmo_shader
           , math::frustum const& frustum
           , const float& cull_distance
           , const math::vector_3d& camera
           , bool draw_fog
           , bool world_has_skies
           , wmo_group_uniform_data& wmo_uniform_data
           , int instance_count
           , noggit::texture_array_handler& texture_handler
           );
  void draw_depth( int instance_count
                 , noggit::texture_array_handler& texture_handler
                 );

  void setupFog (bool draw_fog, std::function<void (bool)> setup_fog);

  void intersect (math::ray const&, std::vector<float>* results) const;

  // todo: portal culling
  bool is_visible( math::matrix_4x4 const& transform_matrix
                 , math::frustum const& frustum
                 , float const& cull_distance
                 , math::vector_3d const& camera
                 , display_mode display
                 ) const;

  bool visible = true;

  std::vector<uint16_t> doodad_ref() const { return _doodad_ref; }

  math::vector_3d BoundingBoxMin;
  math::vector_3d BoundingBoxMax;
  math::vector_3d VertexBoxMin;
  math::vector_3d VertexBoxMax;

  bool use_outdoor_lights;
  std::string name;

  bool has_skybox() const { return header.flags.skybox; }

  std::unique_ptr<wmo_liquid> liquid;

private:
  void load_mocv(MPQFile& f, uint32_t size);
  void fix_vertex_color_alpha();

  WMO *wmo;
  wmo_group_header header;
  ::math::vector_3d center;
  float rad;
  int32_t num;
  int32_t fog;
  std::vector<uint16_t> _doodad_ref;

  std::vector<wmo_batch> _batches;
  std::vector<wmo_render_batch> _render_batches;

  std::vector<::math::vector_3d> _vertices;
  std::vector<::math::vector_3d> _normals;
  std::vector<::math::vector_2d> _texcoords;
  std::vector<::math::vector_2d> _texcoords_2;
  std::vector<::math::vector_4d> _vertex_colors;
  std::vector<uint16_t> _indices;

  GLuint _ubo = 0;

  int _vertex_offset = 0;
  int _index_offset = 0;
};

struct WMOLight
{
  uint32_t flags, color;
  math::vector_3d pos;
  float intensity;
  float unk[5];
  float r;

  math::vector_4d fcolor;

  void init(MPQFile* f);
  void setup(GLint light);

  static void setupOnce(GLint light, math::vector_3d dir, math::vector_3d light_color);
};

struct WMOFog
{
  unsigned int flags;
  math::vector_3d pos;
  float r1, r2, fogend, fogstart;
  unsigned int color1;
  float f2;
  float f3;
  unsigned int color2;
  // read to here (0x30 bytes)
  math::vector_4d color;
  void init(MPQFile* f);
  void setup();
};

class WMO : public AsyncObject
{
public:
  explicit WMO(const std::string& name);

  void draw_instanced ( opengl::scoped::use_program& wmo_shader
                      , math::matrix_4x4 const& model_view
                      , math::matrix_4x4 const& projection
                      , std::vector<WMOInstance*>& instances
                      , bool boundingbox
                      , math::frustum const& frustum
                      , const float& cull_distance
                      , const math::vector_3d& camera
                      , bool draw_doodads
                      , bool draw_fog
                      , liquid_render& render
                      , int animtime
                      , bool world_has_skies
                      , display_mode display
                      , wmo_group_uniform_data& wmo_uniform_data
                      , std::vector<std::pair<wmo_liquid*, math::matrix_4x4>>& wmo_liquids_to_draw
                      , noggit::texture_array_handler& texture_handler
                      , bool update_transform_matrix_buffer
                      );
  void draw_depth ( opengl::scoped::use_program& depth_shader
                  , std::vector<WMOInstance*>& instances
                  , noggit::texture_array_handler& texture_handler
                  );

  void draw_boxes_instanced(opengl::scoped::use_program& wmo_box_shader);

  bool draw_skybox( math::matrix_4x4 const& model_view
                  , math::vector_3d const& camera_pos
                  , opengl::scoped::use_program& m2_shader
                  , math::frustum const& frustum
                  , const float& cull_distance
                  , int animtime
                  , bool draw_particles
                  , math::vector_3d aabb_min
                  , math::vector_3d aabb_max
                  , std::map<int, std::pair<math::vector_3d, math::vector_3d>> const& group_extents
                  , noggit::texture_array_handler& texture_handler
                  ) const;

  std::vector<float> intersect (math::ray const&) const;

  void finishLoading();

  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads_per_group(uint16_t doodadset) const;

  bool draw_group_boundingboxes;

  bool _finished_upload;

  std::vector<WMOGroup> groups;
  std::vector<WMOMaterial> materials;
  math::vector_3d extents[2];

  std::vector<noggit::texture_infos const*> _textures_infos;
  bool _textures_finished_upload = false;

  bool check_texture_upload_status();

  std::vector<std::string> textures;
  std::vector<std::string> models;
  std::vector<wmo_doodad_instance> modelis;
  std::vector<math::vector_3d> model_nearest_light_vector;

  std::vector<WMOLight> lights;
  math::vector_4d ambient_light_color;

  mohd_flags flags;

  std::vector<WMOFog> fogs;

  std::vector<WMODoodadSet> doodadsets;

  std::optional<scoped_model_reference> skybox;

  bool is_hidden() const { return _hidden; }
  void toggle_visibility() { _hidden = !_hidden; }
  void show() { _hidden = false ; }

  virtual bool is_required_when_saving() const
  {
    return true;
  }

  void require_transform_buffer_update() { _need_transform_buffer_update = true; }

  bool has_liquids() const { return _has_liquids; }

private:
  bool _has_liquids = false;

  bool _hidden = false;
  bool _need_transform_buffer_update = true;

  bool _uploaded = false;
  bool _bbox_uploaded = false;

  int _instance_visible = 0;

  opengl::scoped::deferred_upload_buffers<7> _buffers;
  opengl::scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;

  GLuint const& _vao = _vertex_arrays[0];

  GLuint const& _transform_buffer = _buffers[0];
  GLuint const& _vertices_buffer = _buffers[1];
  GLuint const& _indices_buffer = _buffers[2];
  GLuint const& _ubo = _buffers[3];

  GLuint const& _bbox_vao = _vertex_arrays[1];
  GLuint const& _bbox_vertices = _buffers[4];
  GLuint const& _bbox_indices = _buffers[5];

  GLuint const& _depth_transform_buffer = _buffers[6];
  GLuint const& _depth_vao = _vertex_arrays[2];
};

class WMOManager
{
public:
  static void report();
  static void clear_hidden_wmos();
private:
  friend struct scoped_wmo_reference;
  static noggit::async_object_multimap_with_normalized_key<WMO> _;
};

struct scoped_wmo_reference
{
  scoped_wmo_reference (std::string const& filename)
    : _valid (true)
    , _filename (filename)
    , _wmo (WMOManager::_.emplace (_filename))
  {}

  scoped_wmo_reference (scoped_wmo_reference const& other)
    : _valid (other._valid)
    , _filename (other._filename)
    , _wmo (WMOManager::_.emplace (_filename))
  {}
  scoped_wmo_reference& operator= (scoped_wmo_reference const& other)
  {
    _valid = other._valid;
    _filename = other._filename;
    _wmo = WMOManager::_.emplace (_filename);
    return *this;
  }

  scoped_wmo_reference (scoped_wmo_reference&& other)
    : _valid (other._valid)
    , _filename (other._filename)
    , _wmo (other._wmo)
  {
    other._valid = false;
  }
  scoped_wmo_reference& operator= (scoped_wmo_reference&& other)
  {
    std::swap(_valid, other._valid);
    std::swap(_filename, other._filename);
    std::swap(_wmo, other._wmo);
    other._valid = false;
    return *this;
  }

  ~scoped_wmo_reference()
  {
    if (_valid)
    {
      WMOManager::_.erase (_filename);
    }
  }

  WMO* operator->() const
  {
    return _wmo;
  }
  WMO* get() const
  {
    return _wmo;
  }

private:
  bool _valid;

  std::string _filename;
  WMO* _wmo;
};
