// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>
#include <math/matrix_4x4.hpp>
#include <math/quaternion.hpp>
#include <math/ray.hpp>
#include <math/vector_3d.hpp>
#include <noggit/Animated.h> // Animation::M2Value
#include <noggit/AsyncObject.h> // AsyncObject
#include <noggit/MPQ.h>
#include <noggit/ModelHeaders.h>
#include <noggit/Particle.h>
#include <noggit/texture_array_handler.hpp>
#include <noggit/tool_enums.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <string>
#include <vector>

class Bone;
class Model;
class ModelInstance;
class ParticleSystem;
class RibbonEmitter;

math::vector_3d fixCoordSystem(math::vector_3d v);

class opengl_model_state_changer
{
public:
  opengl_model_state_changer();
  ~opengl_model_state_changer();

  void set_depth_mask(bool v);
  void set_cull_face(bool v);
  void set_blend(bool v);
  void set_blend_func(GLenum v1, GLenum v2);

private:
  bool depth_mask;
  bool cull_face;
  bool blend;
  std::pair<GLenum, GLenum> blend_func;
};

class Bone {
  Animation::M2Value<math::vector_3d> trans;
  Animation::M2Value<math::quaternion, math::packed_quaternion> rot;
  Animation::M2Value<math::vector_3d> scale;

public:
  math::vector_3d pivot;
  int parent;

  typedef struct
  {
    uint32_t flag_0x1 : 1;
    uint32_t flag_0x2 : 1;
    uint32_t flag_0x4 : 1;
    uint32_t billboard : 1;
    uint32_t cylindrical_billboard_lock_x : 1;
    uint32_t cylindrical_billboard_lock_y : 1;
    uint32_t cylindrical_billboard_lock_z : 1;
    uint32_t flag_0x80 : 1;
    uint32_t flag_0x100 : 1;
    uint32_t transformed : 1;
    uint32_t unused : 20;
  } bone_flags;

  bone_flags flags;
  math::matrix_4x4 mat = math::matrix_4x4::uninitialized;
  math::matrix_4x4 mrot = math::matrix_4x4::uninitialized;

  bool calc;
  void calcMatrix( math::matrix_4x4 const& model_view
                 , Bone* allbones
                 , int anim
                 , int time
                 , int animtime
                 );
  Bone ( const MPQFile& f,
         const ModelBoneDef &b,
         int *global,
         const std::vector<std::unique_ptr<MPQFile>>& animation_files
       );

};


class TextureAnim {
  Animation::M2Value<math::vector_3d> trans;
  Animation::M2Value<math::quaternion, math::packed_quaternion> rot;
  Animation::M2Value<math::vector_3d> scale;

public:
  math::matrix_4x4 mat;

  void calc(int anim, int time, int animtime);
  TextureAnim(const MPQFile& f, const ModelTexAnimDef &mta, int *global);
};

struct ModelColor {
  Animation::M2Value<math::vector_3d> color;
  Animation::M2Value<float, int16_t> opacity;

  ModelColor(const MPQFile& f, const ModelColorDef &mcd, int *global);
};

struct ModelTransparency {
  Animation::M2Value<float, int16_t> trans;

  ModelTransparency(const MPQFile& f, const ModelTransDef &mtd, int *global);
};


enum class M2Blend : uint16_t
{
  Opaque,
  Alpha_Key,
  Alpha,
  No_Add_Alpha,
  Add,
  Mod,
  Mod2x
};

enum class ModelPixelShader : uint16_t
{
  Combiners_Opaque,
  Combiners_Decal,
  Combiners_Add,
  Combiners_Mod2x,
  Combiners_Fade,
  Combiners_Mod,
  Combiners_Opaque_Opaque,
  Combiners_Opaque_Add,
  Combiners_Opaque_Mod2x,
  Combiners_Opaque_Mod2xNA,
  Combiners_Opaque_AddNA,
  Combiners_Opaque_Mod,
  Combiners_Mod_Opaque,
  Combiners_Mod_Add,
  Combiners_Mod_Mod2x,
  Combiners_Mod_Mod2xNA,
  Combiners_Mod_AddNA,
  Combiners_Mod_Mod,
  Combiners_Add_Mod,
  Combiners_Mod2x_Mod2x,
  Combiners_Opaque_Mod2xNA_Alpha,
  Combiners_Opaque_AddAlpha,
  Combiners_Opaque_AddAlpha_Alpha,
};

enum class texture_unit_lookup : int
{
  environment,
  t1,
  t2,
  none
};


struct m2_render_pass_ubo_data
{
  m2_render_pass_ubo_data() = default;

  math::vector_4d mesh_color;

  int fog_mode;
  int unfogged;
  int unlit;
  int pixel_shader;

  math::matrix_4x4 tex_matrix_1 = math::matrix_4x4(math::matrix_4x4::unit);
  math::matrix_4x4 tex_matrix_2 = math::matrix_4x4(math::matrix_4x4::unit);

  std::uint64_t texture_1;
  std::uint64_t pad1;
  std::uint64_t texture_2;
  std::uint64_t pad2;

  int index_1;
  int index_2;
  int padding[2];

  float alpha_test;
  int tex_unit_lookup_1;
  int tex_unit_lookup_2;
  int tex_count = 1;
};

struct ModelRenderPass : ModelTexUnit
{
  ModelRenderPass() = delete;
  ModelRenderPass(ModelTexUnit const& tex_unit, Model* m);

  float ordering_thingy = 0.f;
  uint16_t index_start = 0, index_count = 0, vertex_start = 0, vertex_end = 0;
  uint16_t blend_mode = 0;
  texture_unit_lookup tu_lookups[2];
  uint16_t textures[2];
  uint16_t uv_animations[2];
  std::optional<ModelPixelShader> pixel_shader;

  m2_render_pass_ubo_data ubo_data;
  bool need_ubo_data_update = true;
  bool render = true;

  bool prepare_draw( opengl::scoped::use_program& m2_shader, Model *m, bool animate, int index
                   , noggit::texture_array_handler& texture_handler, opengl_model_state_changer& ogl_state
                   );
  void after_draw();
  void init_uv_types(Model* m);

  bool operator< (const ModelRenderPass &m) const
  {
    if (priority_plane < m.priority_plane)
    {
      return true;
    }
    else if (priority_plane > m.priority_plane)
    {
      return false;
    }
    else
    {
      return blend_mode == m.blend_mode ? (ordering_thingy < m.ordering_thingy) : blend_mode < m.blend_mode;
    }
  }
};

struct FakeGeometry
{
  FakeGeometry(Model* m);

  std::vector<math::vector_3d> vertices;
  std::vector<uint16_t> indices;
};

struct ModelLight {
  int type, parent;
  math::vector_3d pos, tpos, dir, tdir;
  Animation::M2Value<math::vector_3d> diffColor, ambColor;
  Animation::M2Value<float> diffIntensity, ambIntensity;
  //Animation::M2Value<float> attStart,attEnd;
  //Animation::M2Value<bool> Enabled;

  ModelLight(const MPQFile&  f, const ModelLightDef &mld, int *global);
  void setup(int time, opengl::light l, int animtime);
};

class Model : public AsyncObject
{
public:
  template<typename T>
    static std::vector<T> M2Array(MPQFile const& f, uint32_t offset, uint32_t count)
  {
    T const* start = reinterpret_cast<T const*>(f.getBuffer() + offset);
    return std::vector<T>(start, start + count);
  }

  Model(const std::string& name);

  void draw( math::matrix_4x4 const& model_view
           , ModelInstance& instance
           , opengl::scoped::use_program& m2_shader
           , math::frustum const& frustum
           , const float& cull_distance
           , const math::vector_3d& camera
           , int animtime
           , bool draw_particles
           , bool all_boxes
           , display_mode display
           , noggit::texture_array_handler& texture_handler
           , opengl_model_state_changer& ogl_state
           );
  void draw ( math::matrix_4x4 const& model_view
            , std::vector<ModelInstance*> instances
            , opengl::scoped::use_program& m2_shader
            , math::frustum const& frustum
            , const float& cull_distance
            , const math::vector_3d& camera
            , bool draw_fog
            , int animtime
            , bool draw_particles
            , bool all_boxes
            , std::unordered_map<Model*, std::size_t>& models_with_particles
            , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
            , display_mode display
            , bool update_transform_matrix_buffer
            , noggit::texture_array_handler& texture_handler
            , opengl_model_state_changer& ogl_state
            );
  void draw_particles( math::matrix_4x4 const& model_view
                     , opengl::scoped::use_program& particles_shader
                     , std::size_t instance_count
                     , noggit::texture_array_handler& texture_handler
                     );
  void draw_ribbons( opengl::scoped::use_program& ribbons_shader
                   , std::size_t instance_count
                   , noggit::texture_array_handler& texture_handler
                   );

  void draw_depth( std::vector<ModelInstance*> instances
                 , opengl::scoped::use_program& depth_shader
                 , noggit::texture_array_handler& texture_handler
                 , opengl_model_state_changer& ogl_state
                 );

  void draw_box (opengl::scoped::use_program& m2_box_shader, std::size_t box_count);

  std::vector<float> intersect (math::matrix_4x4 const& model_view, math::ray const&, int animtime);

  void updateEmitters(float dt);

  virtual void finishLoading();

  bool is_hidden() const { return _hidden; }
  void toggle_visibility() { _hidden = !_hidden; }
  void show() { _hidden = false ; }

  bool use_fake_geometry() const { return !!_fake_geometry; }

  virtual bool is_required_when_saving() const
  {
    return true;
  }

  void require_transform_buffer_update() { _need_transform_buffer_update = true; }

  // ===============================
  // Toggles
  // ===============================
  std::vector<bool> showGeosets;

  // ===============================
  // Texture data
  // ===============================
  std::vector<noggit::texture_infos const*> _textures_infos;
  bool _textures_finished_upload = false;

  bool check_texture_upload_status();

  std::vector<std::string> _textureFilenames;
  std::vector<int> _specialTextures;
  std::vector<bool> _useReplaceTextures;
  std::vector<int16_t> _texture_unit_lookup;

  // ===============================
  // Misc ?
  // ===============================
  std::vector<Bone> bones;
  ModelHeader header;
  std::vector<uint16_t> blend_override;

  float rad;
  float trans;
  bool animcalc;

  // parsed asset counts, for load diagnostics
  bool is_vanilla() const { return _vanilla; }
  std::size_t bone_count() const { return bones.size(); }
  std::size_t particle_system_count() const { return _particles.size(); }
  std::size_t ribbon_count() const { return _ribbons.size(); }
  std::size_t light_count() const { return _lights.size(); }

private:
  int _instance_visible = 0;

  bool _per_instance_animation;
  int _current_anim_seq;
  int _anim_time;
  int _global_animtime;

  void initCommon(const MPQFile& f);
  bool isAnimated(const MPQFile& f);
  void initAnimated(const MPQFile& f);

  // vanilla (M2 version < 264) files use different struct layouts; the header is
  // normalized into the WotLK-shaped `header` on load, and the def arrays that
  // embed animation tracks are translated so the rest of the loader stays
  // version agnostic
  void load_vanilla_defs(const MPQFile& f);

  void fix_shader_id_blend_override();
  void fix_shader_id_layer();
  void compute_pixel_shader_ids();

  void animate(math::matrix_4x4 const& model_view, int anim_id, int anim_time);
  void calcBones(math::matrix_4x4 const& model_view, int anim, int time, int animation_time);

  void lightsOn(opengl::light lbase);
  void lightsOff(opengl::light lbase);

  void upload(noggit::texture_array_handler& texture_handler);

  bool _finished_upload;
  bool _need_transform_buffer_update = true;

  std::vector<math::vector_3d> _vertex_box_points;

  // vanilla (M2 version 256/257) state
  bool _vanilla = false;
  uint32_t _skin_profile_ofs = 0;
  std::vector<ModelBoneDef> _vanilla_bone_defs;
  std::vector<ModelColorDef> _vanilla_color_defs;
  std::vector<ModelTransDef> _vanilla_trans_defs;
  std::vector<ModelTexAnimDef> _vanilla_texanim_defs;
  std::vector<ModelLightDef> _vanilla_light_defs;
  std::vector<ModelRibbonEmitterDef> _vanilla_ribbon_defs;
  std::vector<ModelParticleEmitterDef> _vanilla_particle_defs;
  std::vector<vanilla_particle_params> _vanilla_particle_params;

  // buffers;
  opengl::scoped::deferred_upload_buffers<5> _buffers;
  opengl::scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;

  GLuint const& _vao = _vertex_arrays[0];
  GLuint const& _transform_buffer = _buffers[0];
  GLuint const& _vertices_buffer = _buffers[1];
  GLuint const& _ubo = _buffers[2];

  GLuint const& _box_vao = _vertex_arrays[1];
  GLuint const& _box_vbo = _buffers[3];

  GLuint const& _depth_vao = _vertex_arrays[2];
  GLuint const& _depth_transform_buffer = _buffers[4];

  // ===============================
  // Geometry
  // ===============================

  std::vector<ModelVertex> _vertices;
  std::vector<ModelVertex> _current_vertices;

  std::vector<uint16_t> _indices;

  std::vector<ModelRenderPass> _render_passes;
  std::optional<FakeGeometry> _fake_geometry;

  // ===============================
  // Animation
  // ===============================
  bool animated;
  bool animGeometry, animTextures, animBones;

  //      <anim_id, <sub_anim_id, animation>
  std::map<uint16_t, std::map<uint16_t, ModelAnimation>> _animations_seq_per_id;
  std::map<int16_t, uint32_t> _animation_length;

  // vanilla sequences index into one shared animation timeline with
  // start/end timestamps instead of carrying a duration
  std::map<uint16_t, std::map<uint16_t, uint32_t>> _vanilla_seq_start;

  std::vector<ModelRenderFlags> _render_flags;
  std::vector<ParticleSystem> _particles;
  std::vector<RibbonEmitter> _ribbons;

  std::vector<int> _global_sequences;
  std::vector<TextureAnim> _texture_animations;
  std::vector<int16_t> _texture_animation_lookups;
  std::vector<uint16_t> _texture_lookup;

  // ===============================
  // Material
  // ===============================
  std::vector<ModelColor> _colors;
  std::vector<ModelTransparency> _transparency;
  std::vector<int16_t> _transparency_lookup;
  std::vector<ModelLight> _lights;

  bool _hidden = false;

  friend struct ModelRenderPass;
};

