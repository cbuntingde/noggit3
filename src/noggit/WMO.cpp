// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/bounding_box.hpp>
#include <math/frustum.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/Log.h> // LogDebug
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h>
#include <noggit/World.h>
#include <opengl/primitives.hpp>
#include <opengl/scoped.hpp>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <stdexcept>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>


WMO::WMO(const std::string& filenameArg)
  : AsyncObject(filenameArg)
  , _finished_upload(false)
{
}

void WMO::finishLoading ()
{
  MPQFile f(filename);
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << filename << "\"." << std::endl;
    return;
  }

  uint32_t fourcc;
  uint32_t size;

  float ff[3];

  char const* ddnames = nullptr;
  char const* groupnames = nullptr;
  size_t groupnames_size = 0;

  // - MVER ----------------------------------------------

  uint32_t version;

  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" is truncated.");
  }
  f.seekRelative (4);
  if (f.read (&version, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" is truncated.");
  }

  if (fourcc != 'MVER' || version != 17)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an invalid header.");
  }

  // - MOHD ----------------------------------------------

  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" is truncated.");
  }
  f.seekRelative (4);

  if (fourcc != 'MOHD')
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an invalid MOHD chunk.");
  }

  CArgb ambient_color;
  unsigned int nTextures, nGroups, nP, nLights, nModels, nDoodads, nDoodadSets, nX;
  // header
  if (f.read (&nTextures, 4) != 4 || f.read (&nGroups, 4) != 4 || f.read (&nP, 4) != 4
    || f.read (&nLights, 4) != 4 || f.read (&nModels, 4) != 4 || f.read (&nDoodads, 4) != 4
    || f.read (&nDoodadSets, 4) != 4 || f.read (&ambient_color, 4) != 4 || f.read (&nX, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has a truncated MOHD chunk.");
  }
  if (nGroups > 8192U || nTextures > 65536U)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has implausible counts.");
  }
  if (f.read (ff, 12) != 12)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has a truncated MOHD chunk.");
  }
  extents[0] = ::math::vector_3d (ff[0], ff[1], ff[2]);
  if (f.read (ff, 12) != 12)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has a truncated MOHD chunk.");
  }
  extents[1] = ::math::vector_3d (ff[0], ff[1], ff[2]);
  if (f.read(&flags, 2) != 2)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has a truncated MOHD chunk.");
  }

  f.seekRelative (2);

  ambient_light_color.x = static_cast<float>(ambient_color.r) / 255.f;
  ambient_light_color.y = static_cast<float>(ambient_color.g) / 255.f;
  ambient_light_color.z = static_cast<float>(ambient_color.b) / 255.f;
  ambient_light_color.w = static_cast<float>(ambient_color.a) / 255.f;

  // - MOTX ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" is truncated.");
  }

  if (fourcc != 'MOTX')
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an invalid MOTX chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an implausible MOTX size.");
  }

  std::vector<char> texbuf (size);
  if (!texbuf.empty() && f.read (texbuf.data(), texbuf.size()) != texbuf.size())
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has a truncated MOTX chunk.");
  }
  texbuf.push_back ('\0');

  // - MOMT ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO \"" + filename + "\" is truncated.");
  }

  if (fourcc != 'MOMT')
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an invalid MOMT chunk.");
  }
  if (size % 0x40U != 0U || size / 0x40U > 65536U || size > f.remaining())
  {
    throw std::runtime_error ("WMO \"" + filename + "\" has an invalid MOMT size.");
  }

  std::size_t const num_materials (size / 0x40);
  materials.resize (num_materials);

  std::map<std::uint32_t, std::size_t> texture_offset_to_inmem_index;

  auto load_texture
    ( [&] (std::uint32_t ofs)
      {
        char const* texture ("textures/shanecube.blp");
        if (ofs < texbuf.size() && texbuf[ofs])
        {
          size_t const max_len = texbuf.size() - ofs;
          size_t const len = strnlen (&texbuf[ofs], max_len);
          if (len < max_len)
          {
            texture = &texbuf[ofs];
          }
        }

        auto const mapping
          (texture_offset_to_inmem_index.emplace(ofs, textures.size()));

        if (mapping.second)
        {
          textures.emplace_back(texture);
        }
        return mapping.first->second;
      }
    );

  for (size_t i(0); i < num_materials; ++i)
  {
    f.read(&materials[i], sizeof(WMOMaterial));

    uint32_t shader = materials[i].shader;
    bool use_second_texture = (shader == 6 || shader == 5 || shader == 3);

    materials[i].texture1 = load_texture(materials[i].texture_offset_1);
    if (use_second_texture)
    {
      materials[i].texture2 = load_texture(materials[i].texture_offset_2);
    }
  }

  // - MOGN ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOGN')
  {
    throw std::runtime_error ("WMO has an invalid MOGN chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOGN size.");
  }

  groupnames = reinterpret_cast<char const*> (f.getPointer ());
  groupnames_size = size;

  f.seekRelative (size);

  // - MOGI ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOGI')
  {
    throw std::runtime_error ("WMO has an invalid MOGI chunk.");
  }
  if (size > 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOGI size.");
  }

  groups.reserve (nGroups);
  for (size_t i (0); i < nGroups; ++i) {
    groups.emplace_back (this, &f, static_cast<int>(i), groupnames, groupnames_size);
  }

  // - MOSB ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOSB')
  {
    throw std::runtime_error ("WMO has an invalid MOSB chunk.");
  }
  if (size > 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOSB size.");
  }

  if (size > 4)
  {
    f.require_range (f.getPos(), size);
    char const* const skybox_name = reinterpret_cast<char const*>(f.getPointer ());
    size_t const skybox_len = strnlen (skybox_name, size);
    if (skybox_len >= size)
    {
      throw std::runtime_error ("WMO skybox name is not NUL-terminated.");
    }
    std::string path = noggit::mpq::normalized_filename(std::string (skybox_name, skybox_len));
    path = misc::replace(path, "mdx", "m2");

    if (path.length())
    {
      if (MPQFile::exists(path))
      {
        skybox = scoped_model_reference(path);
      }
    }
  }

  f.seekRelative (size);

  // - MOPV ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read(&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOPV')
  {
    throw std::runtime_error ("WMO has an invalid MOPV chunk.");
  }
  if (size % 12U != 0U || size / 12U > 100'000U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an invalid MOPV size.");
  }

  std::vector<math::vector_3d> portal_vertices;

  for (size_t i (0); i < size / 12; ++i) {
    if (f.read (ff, 12) != 12)
    {
      throw std::runtime_error ("WMO has a truncated MOPV chunk.");
    }
    portal_vertices.push_back(math::vector_3d(ff[0], ff[2], -ff[1]));
  }

  // - MOPT ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOPT')
  {
    throw std::runtime_error ("WMO has an invalid MOPT chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOPT size.");
  }

  f.seekRelative (size);

  // - MOPR ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOPR')
  {
    throw std::runtime_error ("WMO has an invalid MOPR chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOPR size.");
  }

  f.seekRelative (size);

  // - MOVV ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOVV')
  {
    throw std::runtime_error ("WMO has an invalid MOVV chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOVV size.");
  }

  f.seekRelative (size);

  // - MOVB ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MOVB')
  {
    throw std::runtime_error ("WMO has an invalid MOVB chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MOVB size.");
  }

  f.seekRelative (size);

  // - MOLT ----------------------------------------------

  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }
  f.seekRelative (4);

  if (fourcc != 'MOLT')
  {
    throw std::runtime_error ("WMO has an invalid MOLT chunk.");
  }

  if (nLights > 1024U)
  {
    throw std::runtime_error ("WMO has an implausible light count.");
  }
  // WMOLight::init reads 48 bytes per light with no size field on MOLT.
  f.require_range (f.getPos(), static_cast<size_t>(nLights) * 48U);
  for (size_t i (0); i < nLights; ++i) {
    WMOLight l;
    l.init (&f);
    lights.push_back (l);
  }

  // - MODS ----------------------------------------------

  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }
  f.seekRelative (4);

  if (fourcc != 'MODS')
  {
    throw std::runtime_error ("WMO has an invalid MODS chunk.");
  }

  if (nDoodadSets > 1024U)
  {
    throw std::runtime_error ("WMO has an implausible doodad set count.");
  }
  f.require_range (f.getPos(), static_cast<size_t>(nDoodadSets) * 32U);
  for (size_t i (0); i < nDoodadSets; ++i) {
    WMODoodadSet dds;
    if (f.read (&dds, 32) != 32)
    {
      throw std::runtime_error ("WMO has a truncated MODS chunk.");
    }
    doodadsets.push_back (dds);
  }

  // - MODN ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MODN')
  {
    throw std::runtime_error ("WMO has an invalid MODN chunk.");
  }
  if (size > 16U * 1024U * 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an implausible MODN size.");
  }

  size_t ddnames_size = 0;
  if (size)
  {
    ddnames = reinterpret_cast<char const*> (f.getPointer ());
    ddnames_size = size;
    f.seekRelative (size);
  }

  // - MODD ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MODD')
  {
    throw std::runtime_error ("WMO has an invalid MODD chunk.");
  }
  if (size % 0x28U != 0U || size / 0x28U > 100'000U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an invalid MODD size.");
  }

  for (size_t i (0); i < size / 0x28; ++i) {
    struct
    {
      uint32_t name_offset : 24;
      uint32_t flag_AcceptProjTex : 1;
      uint32_t flag_0x2 : 1;
      uint32_t flag_0x4 : 1;
      uint32_t flag_0x8 : 1;
      uint32_t flags_unused : 4;
    } x;

    size_t after_entry (f.getPos() + 0x28);
    if (f.read (&x, sizeof (x)) != sizeof (x))
    {
      throw std::runtime_error ("WMO has a truncated MODD chunk.");
    }

    if (!ddnames || x.name_offset >= ddnames_size
      || strnlen (ddnames + x.name_offset, ddnames_size - x.name_offset)
        >= ddnames_size - x.name_offset)
    {
      throw std::runtime_error ("WMO doodad name offset out of range.");
    }
    modelis.emplace_back(ddnames + x.name_offset, &f);
    model_nearest_light_vector.emplace_back();

    f.seek (after_entry);
  }

  // - MFOG ----------------------------------------------

  if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
  {
    throw std::runtime_error ("WMO is truncated.");
  }

  if (fourcc != 'MFOG')
  {
    throw std::runtime_error ("WMO has an invalid MFOG chunk.");
  }
  if (size % 0x30U != 0U || size / 0x30U > 1024U || size > f.remaining())
  {
    throw std::runtime_error ("WMO has an invalid MFOG size.");
  }

  size_t const nfogs = size / 0x30;
  for (size_t i (0); i < nfogs; ++i) {
    WMOFog fog;
    fog.init (&f);
    fogs.push_back (fog);
  }

  for (auto& group : groups)
  {
    group.load();

    if (group.liquid)
    {
      _has_liquids = true;
    }
  }

  finished = true;
  _state_changed.notify_all();
}


void WMO::draw_instanced( opengl::scoped::use_program& wmo_shader
                        , math::matrix_4x4 const& // model_view
                        , math::matrix_4x4 const& // projection
                        , std::vector<WMOInstance*>& instances
                        , bool // boundingbox
                        , math::frustum const& frustum
                        , const float& cull_distance
                        , const math::vector_3d& camera
                        , bool // draw_doodads
                        , bool draw_fog
                        , liquid_render& // render
                        , int // animtime
                        , bool world_has_skies
                        , display_mode display
                        , wmo_group_uniform_data& wmo_uniform_data
                        , std::vector<std::pair<wmo_liquid*, math::matrix_4x4>>& wmo_liquids_to_draw
                        , noggit::texture_array_handler& texture_handler
                        , bool update_transform_matrix_buffer
                        )
{
  if (!finishedLoading() || loading_failed())
  {
    return;
  }

  if (!_uploaded)
  {
    if (_textures_infos.empty())
    {
      for (std::string& tex : textures)
      {
        _textures_infos.push_back(texture_handler.get_texture_info(tex));
      }
    }

    // wait for the textures to load before rendering the model
    // texture data required for the ubo setup
    if (!check_texture_upload_status())
    {
      return;
    }

    _vertex_arrays.upload();
    _buffers.upload();

    opengl::scoped::vao_binder const _ (_vao);

    wmo_shader.attrib(_, "position", _vertices_buffer, 3, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, position));
    wmo_shader.attrib(_, "normal",  _vertices_buffer, 3, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, normal));
    wmo_shader.attrib(_, "color", _vertices_buffer, 4, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, color));
    wmo_shader.attrib(_, "uv1", _vertices_buffer, 2, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, uv1));
    wmo_shader.attrib(_, "uv2", _vertices_buffer, 2, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, uv2));
    wmo_shader.attrib_int(_, "id", _vertices_buffer, 1, GL_INT, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, index));
    wmo_shader.attrib(_, "transform", _transform_buffer, static_cast<math::matrix_4x4*> (nullptr), 1);

    std::vector<wmo_vertex> vertices_data;
    std::vector<std::uint32_t> indices_data;

    for (auto& group : groups)
    {
      group.setup_global_buffer_data(vertices_data, indices_data);
      group.setup_ubo_data();
    }

    gl.bindBuffer(GL_ARRAY_BUFFER, _vertices_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, sizeof(wmo_vertex) * vertices_data.size(), vertices_data.data(), GL_STATIC_DRAW);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indices_buffer);
    gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(std::uint32_t) * indices_data.size(), indices_data.data(), GL_STATIC_DRAW);

    _uploaded = true;
  }

  std::vector<math::matrix_4x4> transform_matrix;

  if (update_transform_matrix_buffer || _need_transform_buffer_update)
  {
    transform_matrix.reserve(instances.size());

    for (WMOInstance* mi : instances)
    {
      if (mi->is_visible(frustum, cull_distance, camera, display))
      {
        transform_matrix.push_back(mi->transform_matrix_transposed());
      }
    }

    _instance_visible = transform_matrix.size();
  }

  if (_instance_visible == 0)
  {
    return;
  }

  if (update_transform_matrix_buffer || _need_transform_buffer_update)
  {
    opengl::scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, _instance_visible * sizeof(::math::matrix_4x4), transform_matrix.data(), GL_STATIC_DRAW);

    _need_transform_buffer_update = false;
  }

  opengl::scoped::vao_binder const _ (_vao);

  wmo_shader.uniform("ambient_color", ambient_light_color.xyz());

  for (auto& group : groups)
  {
    group.draw( wmo_shader
              , frustum
              , cull_distance
              , camera
              , draw_fog
              , world_has_skies
              , wmo_uniform_data
              , _instance_visible
              , texture_handler
              );


    if (group.liquid)
    {
      for (math::matrix_4x4 const& m : transform_matrix)
      {
        wmo_liquids_to_draw.emplace_back(group.liquid.get(), m);
      }
    }
  }
}
void WMO::draw_depth( opengl::scoped::use_program& depth_shader
                    , std::vector<WMOInstance*>& instances
                    , noggit::texture_array_handler& texture_handler
                    )
{
  if (!finishedLoading() || loading_failed() || instances.size() == 0)
  {
    return;
  }

  std::vector<math::matrix_4x4> transform_matrix;
  int instance_visible = 0;

  {
    transform_matrix.reserve(instances.size());

    for (WMOInstance* mi : instances)
    {
      transform_matrix.push_back(mi->transform_matrix_transposed());
    }

    instance_visible = transform_matrix.size();

    opengl::scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_depth_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, instance_visible * sizeof(::math::matrix_4x4), transform_matrix.data(), GL_STATIC_DRAW);
  }

  if (instance_visible == 0)
  {
    return;
  }

  {
    opengl::scoped::vao_binder const _ (_depth_vao);

    {
      opengl::scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);

      depth_shader.attrib(_, "position", opengl::array_buffer_is_already_bound{}, 3, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, position));
      depth_shader.attrib(_, "uv1", opengl::array_buffer_is_already_bound{}, 2, GL_FLOAT, GL_FALSE, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, uv1));
      depth_shader.attrib_int(_, "id", opengl::array_buffer_is_already_bound{}, 1, GL_INT, sizeof(wmo_vertex), (void*)offsetof(wmo_vertex, index));
    }

    {
      opengl::scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_depth_transform_buffer);
      depth_shader.attrib(_, "transform", opengl::array_buffer_is_already_bound{}, static_cast<math::matrix_4x4*> (nullptr), 1);
    }

    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indices_buffer);
  }

  opengl::scoped::vao_binder const _ (_depth_vao);

  for (auto& group : groups)
  {
    group.draw_depth( instance_visible
                    , texture_handler
                    );
  }
}

void WMO::draw_boxes_instanced(opengl::scoped::use_program& wmo_box_shader)
{
  if (!_instance_visible)
  {
    return;
  }

  opengl::scoped::vao_binder const _ (_bbox_vao);

  if (!_bbox_uploaded)
  {
    static std::array<std::uint16_t, 16> const indices
        {{5, 7, 3, 2, 0, 1, 3, 1, 5, 4, 0, 4, 6, 2, 6, 7}};

    std::vector<math::vector_3d> bbox_vertices;
    std::vector<std::uint16_t> bbox_indices;

    bbox_vertices.reserve(groups.size() * 8);
    bbox_indices.reserve(groups.size() * 16);

    for (int i=0; i<groups.size(); ++i)
    {
      auto& group = groups[i];

      auto group_bbox_vertices(math::box_points(group.BoundingBoxMin, group.BoundingBoxMax));
      bbox_vertices.insert(bbox_vertices.begin() + i * 8, group_bbox_vertices.begin(), group_bbox_vertices.end());

      std::uint16_t offset = i * 16;
      for (std::uint16_t indice : indices)
      {
        bbox_indices.push_back(indice + offset);
      }
    }

    gl.bufferData<GL_ARRAY_BUFFER, math::vector_3d>(_bbox_vertices, bbox_vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_bbox_indices, bbox_indices, GL_STATIC_DRAW);

    wmo_box_shader.attrib(_, "position", _bbox_vertices, 3, GL_FLOAT, GL_FALSE, 0, 0);
    wmo_box_shader.attrib(_, "transform", _transform_buffer, static_cast<math::matrix_4x4*> (nullptr), 1);

    gl.bindBuffer(GL_ARRAY_BUFFER, _bbox_vertices);
    gl.bindBuffer(GL_ARRAY_BUFFER, _transform_buffer);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _bbox_indices);

    _bbox_uploaded = true;
  }

  gl.drawElementsInstanced(GL_LINE_STRIP, 16 * groups.size(), _instance_visible, GL_UNSIGNED_SHORT, opengl::index_buffer_is_already_bound{}, 0);
}

std::vector<float> WMO::intersect (math::ray const& ray) const
{
  std::vector<float> results;

  if (!finishedLoading() || loading_failed())
  {
    return results;
  }

  for (auto& group : groups)
  {
    group.intersect (ray, &results);
  }

  return results;
}

bool WMO::draw_skybox ( math::matrix_4x4 const& model_view
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
                      ) const
{
  if (!skybox || !camera_pos.is_inside_of(aabb_min, aabb_max))
  {
    return false;
  }

  for (int i=0; i<groups.size(); ++i)
  {
    auto const& g = groups[i];

    if (!g.has_skybox())
    {
      continue;
    }

    auto& extent(group_extents.at(i));

    if (camera_pos.is_inside_of(extent.first, extent.second))
    {
      ModelInstance sky(skybox.value()->filename);
      sky.set_position(camera_pos);
      sky.set_scale(2.f);
      sky.recalcExtents();

      opengl_model_state_changer model_state_changer;
      skybox->get()->draw(model_view, sky, m2_shader, frustum, cull_distance, camera_pos, animtime, draw_particles, false, display_mode::in_3D, texture_handler, model_state_changer);

      return true;
    }
  }

  return false;
}

bool WMO::check_texture_upload_status()
{
  if (_textures_finished_upload)
  {
    return true;
  }

  for (auto& tex_info : _textures_infos)
  {
    if (!tex_info->ready())
    {
      return false;
    }
  }

  return _textures_finished_upload = true;
}

std::map<uint32_t, std::vector<wmo_doodad_instance>> WMO::doodads_per_group(uint16_t doodadset) const
{
  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads;

  if (doodadset >= doodadsets.size())
  {
    LogError << "Invalid doodadset for instance of wmo " << filename << std::endl;
    return doodads;
  }

  auto const& dset = doodadsets[doodadset];
  uint32_t start = dset.start;
  std::uint64_t const end64 = static_cast<std::uint64_t>(start) + dset.size;

  for (int i = 0; i < groups.size(); ++i)
  {
    for (uint16_t ref : groups[i].doodad_ref())
    {
      // dset bounds come from the file; modelis bounds are authoritative.
      if (static_cast<std::uint64_t>(ref) >= start
        && static_cast<std::uint64_t>(ref) < end64
        && ref < modelis.size())
      {
        doodads[i].push_back(modelis[ref]);
      }
    }
  }

  return doodads;
}

void WMOLight::init(MPQFile* f)
{
  char type[4];
  f->read(&type, 4);
  f->read(&color, 4);
  f->read(pos, 12);
  f->read(&intensity, 4);
  f->read(unk, 4 * 5);
  f->read(&r, 4);

  pos = math::vector_3d(pos.x, pos.z, -pos.y);

  // rgb? bgr? hm
  float fa = ((color & 0xff000000) >> 24) / 255.0f;
  float fr = ((color & 0x00ff0000) >> 16) / 255.0f;
  float fg = ((color & 0x0000ff00) >> 8) / 255.0f;
  float fb = ((color & 0x000000ff)) / 255.0f;

  fcolor = math::vector_4d(fr, fg, fb, fa);
  fcolor *= intensity;
  fcolor.w = 1.0f;

  /*
  // light logging
  gLog("Light %08x @ (%4.2f,%4.2f,%4.2f)\t %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f\t(%d,%d,%d,%d)\n",
  color, pos.x, pos.y, pos.z, intensity,
  unk[0], unk[1], unk[2], unk[3], unk[4], r,
  type[0], type[1], type[2], type[3]);
  */
}

void WMOLight::setup(GLint)
{
  // not used right now -_-
}

void WMOLight::setupOnce(GLint, math::vector_3d, math::vector_3d)
{
  //math::vector_4d position(dir, 0);
  //math::vector_4d position(0,1,0,0);

  //math::vector_4d ambient = math::vector_4d(light_color * 0.3f, 1);
  //math::vector_4d diffuse = math::vector_4d(light_color, 1);


  //gl.enable(light);
}



WMOGroup::WMOGroup(WMO *_wmo, MPQFile* f, int _num, char const* names, size_t names_size)
  : wmo(_wmo)
  , num(_num)
{
  // extract group info from f
  std::uint32_t flags; // not used, the flags are in the group header
  if (f->read(&flags, 4) != 4)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }
  float ff[3];
  if (f->read(ff, 12) != 12)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }
  VertexBoxMax = math::vector_3d(ff[0], ff[1], ff[2]);
  if (f->read(ff, 12) != 12)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }
  VertexBoxMin = math::vector_3d(ff[0], ff[1], ff[2]);
  int nameOfs;
  if (f->read(&nameOfs, 4) != 4)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }

  //! \todo  get proper name from group header and/or dbc?
  if (nameOfs > 0 && names
    && static_cast<size_t>(nameOfs) < names_size
    && strnlen (names + nameOfs, names_size - static_cast<size_t>(nameOfs))
      < names_size - static_cast<size_t>(nameOfs)) {
    name = std::string(names + nameOfs);
  }
  else name = "(no name)";
}

WMOGroup::WMOGroup(WMOGroup const& other)
  : BoundingBoxMin(other.BoundingBoxMin)
  , BoundingBoxMax(other.BoundingBoxMax)
  , VertexBoxMin(other.VertexBoxMin)
  , VertexBoxMax(other.VertexBoxMax)
  , use_outdoor_lights(other.use_outdoor_lights)
  , name(other.name)
  , wmo(other.wmo)
  , header(other.header)
  , center(other.center)
  , rad(other.rad)
  , num(other.num)
  , fog(other.fog)
  , _doodad_ref(other._doodad_ref)
  , _batches(other._batches)
  , _vertices(other._vertices)
  , _normals(other._normals)
  , _texcoords(other._texcoords)
  , _texcoords_2(other._texcoords_2)
  , _vertex_colors(other._vertex_colors)
  , _indices(other._indices)
  // A copy uploads its own buffer later; sharing the name would
  // double-delete it. _ubo default-initializes to 0 (no buffer).
{
  if (other.liquid)
  {
    liquid = std::make_unique<wmo_liquid>(*other.liquid.get());
  }
}

WMOGroup::~WMOGroup()
{
  // Never call GL directly here: groups are destroyed with their WMO on
  // any thread (cache eviction, tile unload) with no GL context current.
  if (_ubo)
  {
    GLuint const ubo (_ubo);
    opengl::defer_gl_delete ([ubo]
      {
        GLuint tmp (ubo);
        gl.deleteBuffers (1, &tmp);
      });
  }
}

namespace
{
  math::vector_4d colorFromInt(unsigned int col)
  {
    GLubyte r, g, b, a;
    a = (col & 0xFF000000) >> 24;
    r = (col & 0x00FF0000) >> 16;
    g = (col & 0x0000FF00) >> 8;
    b = (col & 0x000000FF);
    return math::vector_4d(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
  }

  //! Read and validate a chunk header. Throws on truncation, FourCC
  //! mismatch, or a size running past the end of the file.
  uint32_t read_wmo_chunk_size (MPQFile& f, char const* context, uint32_t expected_fourcc)
  {
    uint32_t fourcc = 0;
    uint32_t size = 0;
    if (f.read (&fourcc, 4) != 4 || f.read (&size, 4) != 4)
    {
      throw std::runtime_error (std::string ("WMO: truncated ") + context + " chunk.");
    }
    if (fourcc != expected_fourcc)
    {
      throw std::runtime_error (std::string ("WMO: invalid ") + context + " chunk.");
    }
    if (size > f.remaining())
    {
      throw std::runtime_error (std::string ("WMO: implausible ") + context + " size.");
    }
    return size;
  }

  //! Read a whole chunk payload into a vector. Throws on truncation,
  //! non-multiple sizes, or implausible element counts.
  template<typename T>
    std::vector<T> read_wmo_chunk_vector (MPQFile& f, uint32_t size, size_t max_elements, char const* context)
  {
    if (size % sizeof (T) != 0U)
    {
      throw std::runtime_error (std::string ("WMO: invalid ") + context + " size.");
    }
    size_t const count = size / sizeof (T);
    if (count > max_elements)
    {
      throw std::runtime_error (std::string ("WMO: implausible ") + context + " count.");
    }
    std::vector<T> values (count);
    if (!values.empty() && f.read (values.data(), size) != size)
    {
      throw std::runtime_error (std::string ("WMO: truncated ") + context + " data.");
    }
    return values;
  }
}

void WMOGroup::load()
{
  // open group file
  std::stringstream curNum;
  curNum << "_" << std::setw (3) << std::setfill ('0') << num;

  std::string fname = wmo->filename;
  fname.insert (fname.find (".wmo"), curNum.str ());

  MPQFile f(fname);
  if (f.isEof()) {
    LogError << "Error loading WMO \"" << fname << "\"." << std::endl;
    return;
  }

  // - MVER ----------------------------------------------

  uint32_t fourcc = 0;
  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }
  f.seekRelative (4);

  uint32_t version = 0;

  if (f.read (&version, 4) != 4)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }

  if (fourcc != 'MVER' || version != 17)
  {
    throw std::runtime_error ("WMO group has an invalid MVER chunk.");
  }

  // - MOGP ----------------------------------------------

  if (f.read (&fourcc, 4) != 4)
  {
    throw std::runtime_error ("WMO group is truncated.");
  }
  f.seekRelative (4);

  if (fourcc != 'MOGP')
  {
    throw std::runtime_error ("WMO group has an invalid MOGP chunk.");
  }

  if (f.read (&header, sizeof (wmo_group_header)) != sizeof (wmo_group_header))
  {
    throw std::runtime_error ("WMO group has a truncated header.");
  }

  if (header.fogs[0] >= wmo->fogs.size())
  {
    throw std::out_of_range ("WMO group fog index out of range.");
  }
  WMOFog &wf = wmo->fogs[header.fogs[0]];
  if (wf.r2 <= 0) fog = -1; // default outdoor fog..?
  else fog = header.fogs[0];

  BoundingBoxMin = ::math::vector_3d (header.box1[0], header.box1[2], -header.box1[1]);
  BoundingBoxMax = ::math::vector_3d (header.box2[0], header.box2[2], -header.box2[1]);

  // - MOPY ----------------------------------------------

  f.seekRelative (read_wmo_chunk_size (f, "MOPY", 'MOPY'));

  // - MOVI ----------------------------------------------

  _indices = read_wmo_chunk_vector<uint16_t> (f, read_wmo_chunk_size (f, "MOVI", 'MOVI'), 1'000'000U, "MOVI");

  // - MOVT ----------------------------------------------

  uint32_t const movt_size (read_wmo_chunk_size (f, "MOVT", 'MOVT'));
  if (movt_size % sizeof (::math::vector_3d) != 0U
    || movt_size / sizeof (::math::vector_3d) > 1'000'000U)
  {
    throw std::runtime_error ("WMO: invalid MOVT size.");
  }
  f.require_range (f.getPos(), movt_size);

  // let's hope it's padded to 12 bytes, not 16...
  ::math::vector_3d const* vertices = reinterpret_cast< ::math::vector_3d const*>(f.getPointer ());

  VertexBoxMin = ::math::vector_3d (9999999.0f, 9999999.0f, 9999999.0f);
  VertexBoxMax = ::math::vector_3d (-9999999.0f, -9999999.0f, -9999999.0f);

  rad = 0;

  for (size_t i = 0; i < movt_size / sizeof (::math::vector_3d); ++i) {
    ::math::vector_3d v (vertices[i].x, vertices[i].z, -vertices[i].y);

    if (v.x < VertexBoxMin.x) VertexBoxMin.x = v.x;
    if (v.y < VertexBoxMin.y) VertexBoxMin.y = v.y;
    if (v.z < VertexBoxMin.z) VertexBoxMin.z = v.z;
    if (v.x > VertexBoxMax.x) VertexBoxMax.x = v.x;
    if (v.y > VertexBoxMax.y) VertexBoxMax.y = v.y;
    if (v.z > VertexBoxMax.z) VertexBoxMax.z = v.z;

    _vertices.push_back (v);
  }

  center = (VertexBoxMax + VertexBoxMin) * 0.5f;
  rad = (VertexBoxMax - center).length () + 300.0f;;

  f.seekRelative (movt_size);

  // - MONR ----------------------------------------------

  _normals = read_wmo_chunk_vector<::math::vector_3d> (f, read_wmo_chunk_size (f, "MONR", 'MONR'), 1'000'000U, "MONR");

  for (auto& n : _normals)
  {
    n = {n.x, n.z, -n.y};
  }

  // - MOTV ----------------------------------------------

  _texcoords = read_wmo_chunk_vector<::math::vector_2d> (f, read_wmo_chunk_size (f, "MOTV", 'MOTV'), 1'000'000U, "MOTV");

  // - MOBA ----------------------------------------------

  _batches = read_wmo_chunk_vector<wmo_batch> (f, read_wmo_chunk_size (f, "MOBA", 'MOBA'), 100'000U, "MOBA");

  // - MOLR ----------------------------------------------
  if (header.flags.has_light)
  {
    f.seekRelative (read_wmo_chunk_size (f, "MOLR", 'MOLR'));
  }
  // - MODR ----------------------------------------------
  if (header.flags.has_doodads)
  {
    _doodad_ref = read_wmo_chunk_vector<std::uint16_t> (f, read_wmo_chunk_size (f, "MODR", 'MODR'), 1'000'000U, "MODR");
  }
  // - MOBN ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.seekRelative (read_wmo_chunk_size (f, "MOBN", 'MOBN'));
  }
  // - MOBR ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    f.seekRelative (read_wmo_chunk_size (f, "MOBR", 'MOBR'));
  }

  if (header.flags.flag_0x400)
  {
    // - MPBV ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MPBV", 'MPBV'));

    // - MPBP ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MPBP", 'MPBP'));

    // - MPBI ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MPBI", 'MPBI'));

    // - MPBG ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MPBG", 'MPBG'));
  }
  // - MOCV ----------------------------------------------
  if (header.flags.has_vertex_color)
  {
    uint32_t const mocv_size (read_wmo_chunk_size (f, "MOCV", 'MOCV'));
    load_mocv(f, mocv_size);
  }
  // - MLIQ ----------------------------------------------
  if (header.flags.has_water)
  {
    uint32_t const mliq_size (read_wmo_chunk_size (f, "MLIQ", 'MLIQ'));
    if (mliq_size < 0x1EU)
    {
      throw std::runtime_error ("WMO: truncated MLIQ header.");
    }

    WMOLiquidHeader hlq;
    if (f.read(&hlq, 0x1E) != 0x1E)
    {
      throw std::runtime_error ("WMO: truncated MLIQ header.");
    }

    if (hlq.material_id >= wmo->materials.size())
    {
      throw std::out_of_range ("WMO liquid material index out of range.");
    }
    liquid = std::make_unique<wmo_liquid> ( &f
                                      , hlq
                                      , wmo->materials[hlq.material_id]
                                      , header.group_liquid
                                      , (bool)wmo->flags.use_liquid_type_dbc_id
                                      , (bool)header.flags.ocean
                                      );

    // creating the wmo liquid doesn't move the position
    f.seekRelative(mliq_size - 0x1E);
  }
  if (header.flags.has_mori_morb)
  {
    // - MORI ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MORI", 'MORI'));

    // - MORB ----------------------------------------------
    f.seekRelative (read_wmo_chunk_size (f, "MORB", 'MORB'));
  }

  // - MOTV ----------------------------------------------
  if (header.flags.has_two_motv)
  {
    _texcoords_2 = read_wmo_chunk_vector<::math::vector_2d> (f, read_wmo_chunk_size (f, "MOTV", 'MOTV'), 1'000'000U, "MOTV");
  }
  // - MOCV ----------------------------------------------
  if (header.flags.use_mocv2_for_texture_blending)
  {
    std::vector<CImVector> const mocv_2 (read_wmo_chunk_vector<CImVector> (f, read_wmo_chunk_size (f, "MOCV", 'MOCV'), 1'000'000U, "MOCV"));

    for (size_t i = 0; i < mocv_2.size(); ++i)
    {
      float alpha = static_cast<float>(mocv_2[i].a) / 255.f;

      // the second mocv is used for texture blending only
      if (header.flags.has_vertex_color)
      {
        if (i >= _vertex_colors.size())
        {
          throw std::runtime_error ("WMO: second MOCV larger than vertex colors.");
        }
        _vertex_colors[i].w = alpha;
      }
      else // no vertex coloring, only texture blending with the alpha
      {
        _vertex_colors.emplace_back(0.f, 0.f, 0.f, alpha);
      }
    }
  }

  //dl_light = 0;
  // "real" lighting?
  if (header.flags.indoor && header.flags.has_vertex_color)
  {
    ::math::vector_3d dirmin(1, 1, 1);
    float lenmin;

    for (auto doodad : _doodad_ref)
    {
      size_t const doodad_index (static_cast<size_t>(doodad));
      if (doodad_index >= wmo->modelis.size())
      {
        throw std::out_of_range ("WMO doodad ref out of range.");
      }
      lenmin = 999999.0f * 999999.0f;
      ModelInstance& mi = wmo->modelis[doodad_index];
      for (unsigned int j = 0; j < wmo->lights.size(); j++)
      {
        WMOLight& l = wmo->lights[j];
        ::math::vector_3d dir = l.pos - mi.position();
        float ll = dir.length_squared ();
        if (ll < lenmin)
        {
          lenmin = ll;
          dirmin = dir;
        }
      }
      wmo->model_nearest_light_vector[doodad_index] = dirmin;
    }

    use_outdoor_lights = false;
  }
  else
  {
    use_outdoor_lights = true;
  }
}

void WMOGroup::load_mocv(MPQFile& f, uint32_t size)
{
  if (size % sizeof(uint32_t) != 0U || size / sizeof(uint32_t) > 1'000'000U)
  {
    throw std::runtime_error ("WMO: invalid MOCV size.");
  }
  f.require_range (f.getPos(), size);
  uint32_t const* colors = reinterpret_cast<uint32_t const*> (f.getPointer());
  _vertex_colors.resize(size / sizeof(uint32_t));

  for (size_t i(0); i < size / sizeof(uint32_t); ++i)
  {
    _vertex_colors[i] = colorFromInt(colors[i]);
  }

  if (wmo->flags.do_not_fix_vertex_color_alpha)
  {
    int interior_batchs_start = 0;

    if (header.transparency_batches_count > 0)
    {
      if (static_cast<size_t>(header.transparency_batches_count) > _batches.size())
      {
        throw std::out_of_range ("WMO transparency batch index out of range.");
      }
      interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
    }

    for (int n = interior_batchs_start; n < _vertex_colors.size(); ++n)
    {
      _vertex_colors[n].w = header.flags.exterior ? 1.f : 0.f;
    }
  }
  else
  {
    fix_vertex_color_alpha();
  }

  // there's no read so this is required
  f.seekRelative(size);
}

void WMOGroup::fix_vertex_color_alpha()
{
  int interior_batchs_start = 0;

  if (header.transparency_batches_count > 0)
  {
    if (static_cast<size_t>(header.transparency_batches_count) > _batches.size())
    {
      throw std::out_of_range ("WMO transparency batch index out of range.");
    }
    interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
  }

  math::vector_4d wmo_ambient_color;

  if (wmo->flags.use_unified_render_path)
  {
    wmo_ambient_color = {0.f, 0.f, 0.f, 0.f};
  }
  else
  {
    wmo_ambient_color = wmo->ambient_light_color;
    // w is not used, set it to 0 to avoid changing the vertex color alpha
    wmo_ambient_color.w = 0.f;
  }

  for (int i = 0; i < _vertex_colors.size(); ++i)
  {
    auto& color = _vertex_colors[i];
    float r = color.x;
    float g = color.y;
    float b = color.z;
    float a = color.w;

    // I removed the color = color/2 because it's just multiplied by 2 in the shader afterward in blizzard's code
    if (i >= interior_batchs_start)
    {
      r += ((r * a / 64.f) - wmo_ambient_color.x);
      g += ((g * a / 64.f) - wmo_ambient_color.y);
      b += ((b * a / 64.f) - wmo_ambient_color.z);
    }
    else
    {
      r -= wmo_ambient_color.x;
      g -= wmo_ambient_color.y;
      b -= wmo_ambient_color.z;

      r = (r * (1.f - a));
      g = (g * (1.f - a));
      b = (b * (1.f - a));
    }

    color.x = std::min(255.f, std::max(0.f, r));
    color.y = std::min(255.f, std::max(0.f, g));
    color.z = std::min(255.f, std::max(0.f, b));
    color.w = 1.f; // default value used in the shader so I simplified it here,
                   // it can be overriden by the 2nd mocv chunk
  }
}

bool WMOGroup::is_visible( math::matrix_4x4 const& transform
                         , math::frustum const& frustum
                         , float const& cull_distance
                         , math::vector_3d const& camera
                         , display_mode display
                         ) const
{
  math::vector_3d pos = transform * center;

  if (!frustum.intersectsSphere(pos, rad))
  {
    return false;
  }

  float dist = display == display_mode::in_3D
    ? (pos - camera).length() - rad
    : std::abs(pos.y - camera.y) - rad;

  return (dist < cull_distance);
}

void WMOGroup::setup_global_buffer_data(std::vector<wmo_vertex>& vertices, std::vector<std::uint32_t>& indices)
{
  if (_batches.empty())
  {
    return;
  }

  _vertex_offset = vertices.size();
  _index_offset = indices.size();

  bool mocv = _vertex_colors.size() > 0;
  bool uv2 = _texcoords_2.size() > 0;

  vertices.reserve(vertices.size() + _vertices.size());
  indices.reserve(indices.size()+ _indices.size());

  for (int i = 0; i < _vertices.size(); ++i)
  {
    wmo_vertex v;

    v.position = _vertices[i];
    v.normal = _normals[i];
    v.color = mocv ? _vertex_colors[i] : math::vector_4d();
    v.uv1 = _texcoords[i];
    v.uv2 = uv2 ? _texcoords_2[i] : math::vector_2d();
    v.index = -1;

    vertices.push_back(v);
  }

  int next_vertex_index = vertices.size();

  for (int i = 0; i < _batches.size(); ++i)
  {
    wmo_batch& batch = _batches[i];
    std::map<std::uint16_t, std::uint32_t> shifted_indices;

    for (int j = batch.index_start; j < batch.index_start + batch.index_count; ++j)
    {
      std::uint16_t id = _indices[j];
      wmo_vertex& v = vertices[id + _vertex_offset];

      // not used or used for this batch
      if (v.index == -1 || v.index == i)
      {
        v.index = i;
        indices.push_back(id+_vertex_offset);
      }
      // duplicate vertex if necessary, or use the new index
      // doesn't affect batch indices start/count as it's just shifting the indices
      else
      {
        auto const& it = shifted_indices.find(id);

        if (it != shifted_indices.end())
        {
          indices.push_back(it->second);
        }
        else
        {
          indices.push_back(next_vertex_index);
          vertices.push_back(v);
          vertices[next_vertex_index].index = i;

          shifted_indices[id] = next_vertex_index;
          next_vertex_index++;
        }
      }
    }
  }

  // data no longer need, only keep the vertices/indices for collisions
  _normals.clear();
  _normals.shrink_to_fit();
  _vertex_colors.clear();
  _vertex_colors.shrink_to_fit();
  _texcoords.clear();
  _texcoords.shrink_to_fit();
  _texcoords_2.clear();
  _texcoords_2.shrink_to_fit();
}


void WMOGroup::setup_ubo_data()
{
  int batch_count = _batches.size();
  int exterior_lit = header.flags.exterior_lit | header.flags.exterior;
  int has_mocv = header.flags.has_vertex_color | header.flags.use_mocv2_for_texture_blending;

  std::vector<wmo_ubo_data> data(batch_count);
#ifdef USE_BINDLESS_TEXTURES
  std::vector<wmo_render_batch> render_batches;
#endif

  for (int i = 0; i < batch_count; ++i)
  {
    wmo_batch& batch = _batches[i];
    WMOMaterial const& mat(wmo->materials.at(batch.texture));
    wmo_ubo_data ubo_data;

    ubo_data.exterior_lit = exterior_lit;
    ubo_data.use_vertex_color = has_mocv;
    ubo_data.shader_id = mat.shader;
    ubo_data.unfogged = mat.flags.unfogged;
    ubo_data.unlit = mat.flags.unlit;

    wmo_render_batch rbg;
    rbg.blend_mode = mat.blend_mode;
    rbg.cull = !mat.flags.unculled;
    rbg.index_start = batch.index_start;
    rbg.index_count = batch.index_count;

    if (mat.blend_mode == 1)
    {
      rbg.blend_mode = 0; // only alpha test change, and it's in the ubo
      ubo_data.alpha_test = 0.878431372f; // 224/255
    }
    else if (mat.blend_mode > 6 || mat.blend_mode == 0)
    {
      ubo_data.alpha_test = -1.f;
    }
    else
    {
      ubo_data.alpha_test = 0.003921568f; // 1/255
    }

#ifdef USE_BINDLESS_TEXTURES
    render_batches.push_back(rbg);
#else
    _render_batches.push_back(rbg);
#endif

    auto& t1_param = wmo->_textures_infos.at(mat.texture1);
#ifdef USE_BINDLESS_TEXTURES
    ubo_data.texture_1 = t1_param->array_handle.value();
#else
    ubo_data.texture_1 = mat.texture1;
#endif
    ubo_data.index_1 = t1_param->pos_in_array->second;

    // only shaders using 2 textures in wotlk
    if (mat.shader == 6 || mat.shader == 5 || mat.shader == 3)
    {
      auto& t2_param = wmo->_textures_infos.at(mat.texture2);
#ifdef USE_BINDLESS_TEXTURES
      ubo_data.texture_2 = t2_param->array_handle.value();
#else
      ubo_data.texture_2 = mat.texture2;
#endif
      ubo_data.index_2 = t2_param->pos_in_array->second;
    }
    else
    {
      ubo_data.texture_2 = 0;
      ubo_data.index_2 = 0;
    }

    data[i] = ubo_data;
  }

#ifdef USE_BINDLESS_TEXTURES
  if (!render_batches.empty())
  {
    wmo_render_batch batch = render_batches[0];

    for (int i = 1; i < render_batches.size(); ++i)
    {
      wmo_render_batch b = render_batches[i];

      if (b.blend_mode == batch.blend_mode && b.cull == batch.cull && b.index_start == batch.index_start + batch.index_count)
      {
        batch.index_count += b.index_count;
      }
      else
      {
        _render_batches.push_back(batch);
        batch = b;
      }
    }
    _render_batches.push_back(batch);
  }
#endif

  gl.genBuffers(1, &_ubo);
  gl.bindBuffer(GL_UNIFORM_BUFFER, _ubo);
  gl.bufferData(GL_UNIFORM_BUFFER, sizeof(wmo_ubo_data) * batch_count, data.data(), GL_STATIC_DRAW);
  gl.bindBuffer(GL_UNIFORM_BUFFER, 0);
}

void WMOGroup::draw( opengl::scoped::use_program&
                   , math::frustum const& // frustum
                   , const float& //cull_distance
                   , const math::vector_3d& //camera
                   , bool // draw_fog
                   , bool // world_has_skies
                   , wmo_group_uniform_data& wmo_uniform_data
                   , int instance_count
                   , [[maybe_unused]] noggit::texture_array_handler& texture_handler
                   )
{
  gl.bindBufferBase(GL_UNIFORM_BUFFER, 0, _ubo);

#ifndef USE_BINDLESS_TEXTURES
  int batch_index = 0;
#endif

  for(wmo_render_batch& batch : _render_batches)
  {
    if (batch.blend_mode != wmo_uniform_data.blend_mode)
    {
      switch (batch.blend_mode)
      {
      default:
      case 0:
      case 1:
        gl.disable(GL_BLEND);
        break;
      case 2:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
      case 3:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
      case 4:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_ZERO);
        break;
      case 5:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        break;
      case 6:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_ONE);
        break;
      }

      wmo_uniform_data.blend_mode = batch.blend_mode;
    }

    if (batch.cull != wmo_uniform_data.cull)
    {
      if (!batch.cull)
      {
        gl.disable(GL_CULL_FACE);
      }
      else
      {
        gl.enable(GL_CULL_FACE);
      }

      wmo_uniform_data.cull = batch.cull;
    }

#ifndef USE_BINDLESS_TEXTURES
    WMOMaterial const& mat(wmo->materials.at(_batches[batch_index++].texture));

    texture_handler.bind_layer(wmo->_textures_infos[mat.texture1]->pos_in_array->first, 0);

    if (mat.shader == 3 || mat.shader == 5 || mat.shader == 6)
    {
      texture_handler.bind_layer(wmo->_textures_infos[mat.texture2]->pos_in_array->first, 1);
    }
#endif

    gl.drawElementsInstanced(GL_TRIANGLES, batch.index_count, instance_count, GL_UNSIGNED_INT, opengl::index_buffer_is_already_bound{}, sizeof(std::uint32_t) * (batch.index_start + _index_offset));
  }
}


void WMOGroup::draw_depth( int instance_count
                         , [[maybe_unused]] noggit::texture_array_handler& texture_handler
                         )
{
  gl.bindBufferBase(GL_UNIFORM_BUFFER, 0, _ubo);

#ifndef USE_BINDLESS_TEXTURES
  int batch_index = 0;
#endif

  for(wmo_render_batch& batch : _render_batches)
  {
    // skip transparent batches
    if (batch.blend_mode != 0 && batch.blend_mode != 1)
    {
#ifndef USE_BINDLESS_TEXTURES
      batch_index++;
#endif
      continue;
    }


#ifndef USE_BINDLESS_TEXTURES
    WMOMaterial const& mat(wmo->materials.at(_batches[batch_index++].texture));

    texture_handler.bind_layer(wmo->_textures_infos[mat.texture1]->pos_in_array->first, 0);
#endif

    gl.drawElementsInstanced(GL_TRIANGLES, batch.index_count, instance_count, GL_UNSIGNED_INT, opengl::index_buffer_is_already_bound{}, sizeof(std::uint32_t) * (batch.index_start + _index_offset));
  }
}

void WMOGroup::intersect (math::ray const& ray, std::vector<float>* results) const
{
  if (!ray.intersect_bounds (VertexBoxMin, VertexBoxMax))
  {
    return;
  }

  //! \todo Also allow clicking on doodads and liquids.
  for (auto&& batch : _batches)
  {
    for (size_t i (batch.index_start); i < batch.index_start + batch.index_count; i += 3)
    {
      if ( auto&& distance
         = ray.intersect_triangle ( _vertices[_indices[i + 0]]
                                  , _vertices[_indices[i + 1]]
                                  , _vertices[_indices[i + 2]]
                                  )
         )
      {
        results->emplace_back (*distance);
      }
    }
  }
}

void WMOGroup::setupFog (bool draw_fog, std::function<void (bool)> setup_fog)
{
  if (use_outdoor_lights || fog == -1) {
    setup_fog (draw_fog);
  }
  else {
    wmo->fogs[fog].setup();
  }
}

void WMOFog::init(MPQFile* f)
{
  f->read(this, 0x30);
  color = math::vector_4d(((color1 & 0x00FF0000) >> 16) / 255.0f, ((color1 & 0x0000FF00) >> 8) / 255.0f,
    (color1 & 0x000000FF) / 255.0f, ((color1 & 0xFF000000) >> 24) / 255.0f);
  float temp;
  temp = pos.y;
  pos.y = pos.z;
  pos.z = -temp;
  fogstart = fogstart * fogend * 1.5f;
  fogend *= 1.5;
}

void WMOFog::setup()
{

}

decltype (WMOManager::_) WMOManager::_;

void WMOManager::report()
{
  std::string output = "Still in the WMO manager:\n";
  _.apply ( [&] (std::string const& key, WMO const&)
            {
              output += " - " + key + "\n";
            }
          );
  LogDebug << output;
}

void WMOManager::clear_hidden_wmos()
{
  _.apply ( [&] (std::string const&, WMO& wmo)
            {
              wmo.show();
            }
          );
}
