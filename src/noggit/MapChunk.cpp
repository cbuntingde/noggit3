// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/frustum.hpp>
#include <math/quaternion.hpp>
#include <math/vector_3d.hpp>
#include <noggit/Brush.h>
#include <noggit/chunk_mover.hpp>
#include <noggit/liquid_tile.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/Misc.h>
#include <noggit/World.h>
#include <noggit/alphamap.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <opengl/scoped.hpp>

#include <algorithm>
#include <iostream>
#include <map>

MapChunk::MapChunk(MapTile *maintile, MPQFile *f, bool bigAlpha, tile_mode mode)
  : _mode(mode)
  , mt(maintile)
  , use_big_alphamap(bigAlpha)
{
  uint32_t fourcc;
  uint32_t size;

  size_t base = f->getPos();

  _has_mccv = false;

  // - MCNK ----------------------------------------------
  {
    f->read(&fourcc, 4);
    f->read(&size, 4);

    assert(fourcc == 'MCNK');

    f->read(&header, 0x80);

    _area_id = header.areaid;

    px = header.ix;
    py = header.iy;

    zbase = maintile->index.z * TILESIZE + py * CHUNKSIZE;
    xbase = maintile->index.x * TILESIZE + px * CHUNKSIZE;
    ybase = header.ypos;

    // todo: mark ADT as changed if the values are significantly different (eg: when the ADT was moved)
    header.xpos = ZEROPOINT - xbase;
    header.zpos = ZEROPOINT - zbase;

    _4x4_holes = header.holes;

    vmin = math::vector_3d(9999999.0f, 9999999.0f, 9999999.0f);
    vmax = math::vector_3d(-9999999.0f, -9999999.0f, -9999999.0f);
  }

  // - MCVT ----------------------------------------------
  {
    f->seek(base + header.ofsHeight);
    f->read(&fourcc, 4);
    f->read(&size, 4);

    assert(fourcc == 'MCVT');

    chunk_vertex* cv_ptr = vertices.data();

    // vertices
    for (int j = 0; j < 17; ++j)
    {
      for (int i = 0; i < ((j % 2) ? 8 : 9); ++i)
      {
        float h, xpos, zpos;
        f->read(&h, 4);
        xpos = i * UNITSIZE;
        zpos = j * 0.5f * UNITSIZE;
        if (j % 2)
        {
          xpos += UNITSIZE * 0.5f;
        }
        cv_ptr->position = math::vector_3d(xbase + xpos, ybase + h, zbase + zpos);

        vmin.y = std::min(vmin.y, cv_ptr->position.y);
        vmax.y = std::max(vmax.y, cv_ptr->position.y);

        cv_ptr++;
      }
    }

    vmin.x = xbase;
    vmin.z = zbase;
    vmax.x = xbase + 8 * UNITSIZE;
    vmax.z = zbase + 8 * UNITSIZE;

    update_intersect_points();

    // use absolute y pos in vertices
    ybase = 0.0f;
    header.ypos = 0.0f;
  }
  // - MCNR ----------------------------------------------
  {
    f->seek(base + header.ofsNormal);
    f->read(&fourcc, 4);
    f->read(&size, 4);

    assert(fourcc == 'MCNR');

    char nor[3];
    chunk_vertex* cv_ptr = vertices.data();
    for (int i = 0; i< mapbufsize; ++i)
    {
      f->read(nor, 3);
      cv_ptr->normal = math::vector_3d(nor[0] / 127.0f, nor[2] / 127.0f, nor[1] / 127.0f);
      cv_ptr++;
    }
  }
  // - MCSH ----------------------------------------------
  if(header.ofsShadow && header.sizeShadow)
  {
    f->seek(base + header.ofsShadow);
    f->read(&fourcc, 4);
    f->read(&size, 4);

    assert(fourcc == 'MCSH');

    _chunk_shadow = std::make_unique<chunk_shadow>();

    // shadow map 64 x 64
    f->read(_chunk_shadow.get(), 0x200);
    f->seekRelative(-0x200);

    if (!header.flags.flags.do_not_fix_alpha_map)
    {
      auto& sh_map = _chunk_shadow->data;

      for (std::size_t i(0); i < 64; ++i)
      {
        misc::set_bit(sh_map[i], 63, 0, (sh_map[i] >> 62 & 1));
        misc::set_bit(sh_map[63], i, 0, (sh_map[62] >> i & 1));
      }
      misc::set_bit(sh_map[63], 63, 0, (sh_map[62] >> 62 & 1));
    }

    // no need to use an empty shadowmap
    if (shadow_map_is_empty())
    {
      clear_shadows();
    }
  }

  texture_set = std::make_unique<TextureSet>(header, f, base, maintile, bigAlpha, !!header.flags.flags.do_not_fix_alpha_map, mode == tile_mode::uid_fix_all, _chunk_shadow ? _chunk_shadow.get() : nullptr);

  // - MCCV ----------------------------------------------
  if(header.ofsMCCV)
  {
    f->seek(base + header.ofsMCCV);
    f->read(&fourcc, 4);
    f->read(&size, 4);

    assert(fourcc == 'MCCV');

    if (!(header.flags.flags.has_mccv))
    {
      header.flags.flags.has_mccv = 1;
    }

    _has_mccv = true;

    unsigned char t[4];
    for (int i = 0; i < mapbufsize; ++i)
    {
      f->read(t, 4);
      vertices[i].color = math::vector_3d((float)t[2] / 127.0f, (float)t[1] / 127.0f, (float)t[0] / 127.0f);
    }
  }
  else
  {
    math::vector_3d mccv_default(1.f, 1.f, 1.f);
    for (int i = 0; i < mapbufsize; ++i)
    {
      vertices[i].color = mccv_default;
    }
  }

  if (header.sizeLiquid > 8)
  {
    f->seek(base + header.ofsLiquid);

    f->read(&fourcc, 4);
    f->seekRelative(4); // ignore the size here, the valid size is in the header

    assert(fourcc == 'MCLQ');

    int layer_count = (header.sizeLiquid - 8) / sizeof(mclq);
    std::vector<mclq> layers(layer_count);
    f->read(layers.data(), sizeof(mclq)*layer_count);

    mt->Water.getChunk(px, py)->from_mclq(layers);
    // remove the liquid flags as it'll be saved as MH2O
    header.flags.value &= ~(0xF << 2);
  }

  // no need to create indexes when applying the uid fix
  if (_mode == tile_mode::edit)
  {
    initStrip();
  }

  vcenter = (vmin + vmax) * 0.5f;
}


noggit::chunk_data MapChunk::get_chunk_data()
{
  noggit::chunk_data data;

  data.origin = math::vector_3d(xbase, ybase, zbase);
  std::memcpy(&data.vertices, vertices.data(), mapbufsize * sizeof(chunk_vertex));
  data.area_id = _area_id;
  data.holes = _4x4_holes;
  data.flags = header.flags;
  data.world_id_x = mt->index.x * 16 + px;
  data.world_id_z = mt->index.z * 16 + py;
  data.use_vertex_colors = _has_mccv;

  if (_chunk_shadow)
  {
    data.shadows.emplace();
    std::memcpy(&data.shadows.value().data, _chunk_shadow.get(), sizeof(chunk_shadow));
  }

  std::memcpy(data.low_quality_texture_map.data(), header.low_quality_texture_map,  8 * 2);
  std::memcpy(data.disable_doodads_map.data(), header.disable_doodads_map, 8);

  texture_set->copy_data(data);
  liquid_chunk()->copy_data(data);

  return data;
}

void MapChunk::override_data(noggit::chunk_data& data, noggit::chunk_override_params const& params)
{
  if (params.height && params.vertex_colors && params.height_override_mode == noggit::chunk_override_params::height_mode::normal)
  {
    vertices = data.vertices;
  }
  else
  {
    if (params.height)
    {
      for (int i = 0; i < mapbufsize; ++i)
      {
        float h_orig = vertices[i].position.y, h_new = data.vertices[i].position.y, h = 0.f;

        switch (params.height_override_mode)
        {
          case noggit::chunk_override_params::height_mode::normal:    h = h_new; break;
          case noggit::chunk_override_params::height_mode::min:       h = std::min(h_orig, h_new); break;
          case noggit::chunk_override_params::height_mode::max:       h = std::max(h_orig, h_new); break;
          case noggit::chunk_override_params::height_mode::add:       h = h_orig + h_new; break;
          case noggit::chunk_override_params::height_mode::subtract: h = h_orig - h_new; break;
        }

        vertices[i].position.y = h;
        vertices[i].normal = data.vertices[i].normal;
      }
    }
    if (params.vertex_colors)
    {
      for (int i = 0; i < mapbufsize; ++i)
      {
        vertices[i].color = data.vertices[i].color;
      }
    }
  }

  if (params.area_id)
  {
    _area_id = data.area_id;
  }
  if (params.holes)
  {
    _4x4_holes = data.holes;
  }

  header.flags = data.flags;

  if (data.shadows && params.shadows && !params.clear_shadows)
  {
    _chunk_shadow = std::make_unique<chunk_shadow>();
    std::memcpy(_chunk_shadow->data.data(), data.shadows->data.data(), sizeof(chunk_shadow));
  }
  else if(params.shadows || params.clear_shadows)
  {
    _chunk_shadow.reset();
  }

  // todo: make its own thing ?
  if (params.alphamaps)
  {
    std::memcpy(header.low_quality_texture_map, data.low_quality_texture_map.data(), 8 * 2);
    std::memcpy(header.disable_doodads_map, data.disable_doodads_map.data(), 8);
  }

  texture_set->override_data(data, params);

  if (params.liquids)
  {
    liquid_chunk()->override_data(data, params);
  }

  // force update
  _need_indice_buffer_update = true;
  _need_lod_update = true;
  _need_vao_update = true;
  _need_visibility_update = true;
  _shader_data_need_update = true;

  _has_mccv = data.use_vertex_colors;

  updateVerticesData();
  texture_set_changed();
  initStrip();

  mt->chunk_height_changed();
}
void MapChunk::set_preview_data(noggit::chunk_data& data, noggit::chunk_override_params const& params)
{
  if (!params.preview_terrain_changes)
  {
    _preview_data.reset();
    _preview_params.reset();
    return;
  }

  _preview_data = std::make_unique<noggit::chunk_data>(data);
  _preview_params = std::make_unique<noggit::chunk_override_params>(params);

  if (!params.height && !params.vertex_colors)
  {
    _preview_data->vertices = vertices;
  }
  else if (!params.height)
  {
    for (int i = 0; i < mapbufsize; ++i)
    {
      _preview_data->vertices[i].position = vertices[i].position;
      _preview_data->vertices[i].normal = vertices[i].normal;
    }
  }
  else if (!params.vertex_colors)
  {
    for (int i = 0; i < mapbufsize; ++i)
    {
      _preview_data->vertices[i].color = vertices[i].color;
    }
  }

  if (params.height && params.height_override_mode != noggit::chunk_override_params::height_mode::normal)
  {
    for (int i = 0; i < mapbufsize; ++i)
    {
      float h_orig = vertices[i].position.y, h_new = data.vertices[i].position.y, h = 0.f;
      switch (params.height_override_mode)
      {
        case noggit::chunk_override_params::height_mode::min: h = std::min(h_orig, h_new); break;
        case noggit::chunk_override_params::height_mode::max: h = std::max(h_orig, h_new); break;
        case noggit::chunk_override_params::height_mode::add:       h = h_orig + h_new; break;
        case noggit::chunk_override_params::height_mode::subtract: h = h_orig - h_new; break;
      }

      _preview_data->vertices[i].position.y = h;
    }
  }

  if (!params.area_id)
  {
    _preview_data->area_id = _area_id;
  }
  if (!params.holes)
  {
    _preview_data->holes = _4x4_holes;
  }

  if (params.clear_shadows)
  {
    _preview_data->shadows.reset();
  }
  else if (!params.shadows)
  {
    if (_chunk_shadow)
    {
      _preview_data->shadows.emplace();
      std::memcpy(_preview_data->shadows->data.data(), _chunk_shadow->data.data(), sizeof(chunk_shadow));
    }
    else
    {
      _preview_data->shadows.reset();
    }
  }

  texture_set->require_update();

  if (params.liquids && data.liquid_layer_count > 0)
  {
    liquid_chunk()->set_preview_data(data, params);
  }

  // force update
  _need_indice_buffer_update = true;
  _need_lod_update = true;
  _need_vao_update = true;
  _need_visibility_update = true;
  _shader_data_need_update = true;

  updateVerticesData();
  texture_set_changed();
  initStrip();

  mt->chunk_height_changed();
}

void MapChunk::set_copied(bool v)
{
  _is_copied = v;
  require_shader_data_update();
}
void MapChunk::set_is_in_paste_zone(bool v)
{
  _is_in_paste_zone = v;
  require_shader_data_update();

  if (!v && _preview_data)
  {
    _preview_data.reset();
    _preview_params.reset();

    // force update
    _need_indice_buffer_update = true;
    _need_lod_update = true;
    _need_vao_update = true;
    _need_visibility_update = true;
    _shader_data_need_update = true;

    updateVerticesData();
    texture_set_changed();
    texture_set->require_update();
    liquid_chunk()->clear_preview();
    initStrip();
  }
}

int MapChunk::indexLoD(int z, int x)
{
  return (z + 1) * 9 + z * 8 + x;
}

int MapChunk::indexNoLoD(int z, int x)
{
  return z * 8 + z * 9 + x;
}

void MapChunk::update_intersect_points()
{
  // update the center of the chunk and visibility when the vertices changed
  vcenter = (vmin + vmax) * 0.5f;
  _need_visibility_update = true;

  _intersect_points.clear();
  _intersect_points = misc::intersection_points(vmin, vmax);

  mt->need_chunk_data_update();
}

int MapChunk::get_lod_level(math::vector_3d const& camera_pos, display_mode display) const
{
  // use lod 4 (single quad) only for flat chunks without more than 1 textures
  if (std::abs(vmin.y - vmax.y) < 0.1f && texture_set->nTextures < 2 && !_has_mccv)
  {
    return std::min(lod_count, 4);
  }

  float dist = display == display_mode::in_2D
             ? std::abs(camera_pos.y - vcenter.y)
             : (camera_pos - vcenter).length();

  if (dist < 1000.f)
  {
    return 0;
  }
  else
  {
    // limit lods level to 3 for non-flat/textured chunks
    return std::min(3, 1 + std::min(lod_count - 1, static_cast<int>(dist / 2000.f)));
  }
}

bool MapChunk::shadow_map_is_empty() const
{
  if (!_chunk_shadow)
  {
    return true;
  }

  for (int i = 0; i < 64; ++i)
  {
    if (_chunk_shadow->data[i])
    {
      return false;
    }
  }

  return true;
}

int MapChunk::indices_count(int lod_level) const
{
  int count = 0;

  if (lod_level == 0)
  {
    for (int x = 0; x < 8; ++x)
    {
      for (int y = 0; y < 8; ++y)
      {
        if (!isHole(x / 2, y / 2))
        {
          count += 12;
        }
      }
    }
  }
  else
  {
    int step = 1 << (lod_level - 1);

    for (int x = 0; x < 8; x += step)
    {
      for (int y = 0; y < 8; y += step)
      {
        if (!isHole(x / 2, y / 2))
        {
          count += 6;
        }
      }
    }
  }

  return count;
}

std::vector<chunk_indice> MapChunk::strip_without_holes = {};

void MapChunk::initStrip()
{
  bool init_strip_without_holes = strip_without_holes.size() == 0;

  _indice_strips.clear();

  std::array<int, indice_buffer_count> index_count;

  for (int lod_level = 0; lod_level < indice_buffer_count; ++lod_level)
  {
    int count = indices_count(lod_level);
    _indices_count_per_lod_level[lod_level] = count;
    _indice_strips[lod_level] = std::vector<chunk_indice>(count, 0);
    index_count[lod_level] = 0;
  }

  for (int x = 0; x<8; ++x)
  {
    for (int y = 0; y<8; ++y)
    {
      if (init_strip_without_holes)
      {
        strip_without_holes.emplace_back(indexLoD(y, x)); //9
        strip_without_holes.emplace_back(indexNoLoD(y, x)); //0
        strip_without_holes.emplace_back(indexNoLoD(y + 1, x)); //17
        strip_without_holes.emplace_back(indexLoD(y, x)); //9
        strip_without_holes.emplace_back(indexNoLoD(y + 1, x)); //17
        strip_without_holes.emplace_back(indexNoLoD(y + 1, x + 1)); //18
        strip_without_holes.emplace_back(indexLoD(y, x)); //9
        strip_without_holes.emplace_back(indexNoLoD(y + 1, x + 1)); //18
        strip_without_holes.emplace_back(indexNoLoD(y, x + 1)); //1
        strip_without_holes.emplace_back(indexLoD(y, x)); //9
        strip_without_holes.emplace_back(indexNoLoD(y, x + 1)); //1
        strip_without_holes.emplace_back(indexNoLoD(y, x)); //0
      }

      if (isHole(x / 2, y / 2))
        continue;

      // todo: better hole check ?
      for (int lod_level = 1; lod_level < indice_buffer_count; ++lod_level)
      {
        int n = 1 << (lod_level-1);
        if ((x % n) == 0 && (y % n) == 0)
        {
          int current_index = index_count[lod_level];

          _indice_strips[lod_level][current_index + 0] = (vertex_offset() + indexNoLoD(y, x)); //0
          _indice_strips[lod_level][current_index + 1] = (vertex_offset() + indexNoLoD(y + n, x)); //17
          _indice_strips[lod_level][current_index + 2] = (vertex_offset() + indexNoLoD(y + n, x + n)); //18
          _indice_strips[lod_level][current_index + 3] = (vertex_offset() + indexNoLoD(y + n, x + n)); //18
          _indice_strips[lod_level][current_index + 4] = (vertex_offset() + indexNoLoD(y, x + n)); //1
          _indice_strips[lod_level][current_index + 5] = (vertex_offset() + indexNoLoD(y, x)); //0

          index_count[lod_level] += 6;
        }
      }

      int start = vertex_offset() + indexNoLoD(y, x);
      int current_index = index_count[0];

      static std::array<int, 12> triangles = {{ 9,0,17, 9,17,18, 9,18,1, 9,1,0 }};

      for (int i = 0; i < 12; ++i)
      {
        _indice_strips[0][current_index + i] = start + triangles[i];
      }

      index_count[0] += 12;
    }
  }

  _need_indice_buffer_update = true;

  mt->need_chunk_data_update();
}

bool MapChunk::GetVertex(float x, float z, math::vector_3d *V)
{
  float xdiff, zdiff;

  xdiff = x - xbase;
  zdiff = z - zbase;

  const int row = static_cast<int>(zdiff / (UNITSIZE * 0.5f) + 0.5f);
  const int column = static_cast<int>((xdiff - UNITSIZE * 0.5f * (row % 2)) / UNITSIZE + 0.5f);
  if ((row < 0) || (column < 0) || (row > 16) || (column >((row % 2) ? 8 : 9)))
    return false;

  *V = vertices[17 * (row / 2) + ((row % 2) ? 9 : 0) + column].position;
  return true;
}

float MapChunk::getHeight(int x, int z)
{
  if (x > 9 || z > 9 || x < 0 || z < 0) return 0.0f;
  return vertices[indexNoLoD(x, z)].position.y;
}

std::optional<float> MapChunk::get_exact_height_at(math::vector_3d const& pos)
{
  if (pos.x < vmin.x || pos.x > vmax.x || pos.z < vmin.z || pos.z > vmax.z)
  {
    return std::nullopt;
  }

  // put the ray above the max height to be sure always hit the terrain
  math::ray ray({pos.x, vmax.y + 1.f, pos.z}, {0.f, -1.f, 0.f});

  float diff_x = pos.x - xbase;
  float diff_z = pos.z - zbase;

  int idx = static_cast<int>(diff_x / UNITSIZE);
  int idz = static_cast<int>(diff_z / UNITSIZE);

  float dx = std::fmod(diff_x, UNITSIZE);
  float dz = std::fmod(diff_z, UNITSIZE);

  int id_0 = dx > dz
           ? indexNoLoD(idz, idx+1)
           : indexNoLoD(idz+1, idx)
           ;
  int id_1 = (UNITSIZE - dx) > dz
           ? indexNoLoD(idz, idx)
           : indexNoLoD(idz+1, idx+1)
           ;
  int id_center = indexLoD(idz, idx);

  auto dist = ray.intersect_triangle(vertices[id_0].position, vertices[id_1].position, vertices[id_center].position);

  if (dist)
  {
    return ray.position(dist.value()).y;
  }
  else
  {
    return std::nullopt;
  }
}

void MapChunk::clearHeight()
{
  for (int i = 0; i < mapbufsize; ++i)
  {
    vertices[i].position.y = 0.0f;
    vertices[i].normal = { 0.f, 1.f, 0.f };
  }

  vmin.y = 0.0f;
  vmax.y = 0.0f;

  update_intersect_points();

  require_vertices_buffer_update();

  mt->need_chunk_data_update();
}

bool MapChunk::is_visible ( const float& cull_distance
                          , const math::frustum& frustum
                          , const math::vector_3d& camera
                          , display_mode display
                          ) const
{
  static const float chunk_radius = std::sqrt (CHUNKSIZE * CHUNKSIZE / 2.0f); //was (vmax - vmin).length() * 0.5f;

  float dist = display == display_mode::in_3D
             ? (camera - vcenter).length() - chunk_radius
             : std::abs(camera.y - vmax.y);

  return dist < cull_distance && frustum.intersects (_intersect_points);
}

void MapChunk::set_visible()
{
  _is_visible = true;
  _need_visibility_update = false;
}

void MapChunk::update_visibility ( const float& cull_distance
                                 , const math::frustum& frustum
                                 , const math::vector_3d& camera
                                 , display_mode display
                                 )
{
  auto lod = get_lod_level(camera, display);

  _is_visible = is_visible(cull_distance, frustum, camera, display);
  _need_visibility_update = false;
  _need_lod_update |= lod != _lod_level;
  _lod_level = lod;
}

void MapChunk::require_shader_data_update()
{
  _shader_data_need_update = true;
  mt->need_chunk_data_update();
}

void MapChunk::texture_set_changed()
{
  mt->require_regular_alphamap();
  require_shader_data_update();

  _texture_set_need_update = true;
}

void MapChunk::update_shader_data ( bool selected_texture_changed
                                  , std::string const& current_texture
                                  , std::map<int, misc::random_color>& area_id_colors
                                  , noggit::tileset_array_handler& tileset_handler
                                  , bool
                                  )
{
  chunk_shader_data csd;
  int texture_count = 0;

  if (_preview_data && _preview_params->textures)
  {
    texture_count = _preview_data->texture_count;

    if (texture_count)
    {
      if (!mt->use_no_alpha_alphamap())
      {
        update_alpha_shadow_map();
      }

      for (int i = 0; i < texture_count; ++i)
      {
        if (_texture_set_need_update)
        {
          std::pair<int, int> param = tileset_handler.get_texture_position(_preview_data->textures[i]);

          csd.tex_array_index[i] = param.first;
          csd.tex_index_in_array[i] = param.second;
          csd.tex_animations[i] = math::vector_4d(misc::texture_anim_params(_preview_data->texture_flags[i].flags), 0.f);
        }
        else // no change, reuse the old values
        {
          csd.tex_array_index[i] = _shader_data.tex_array_index[i];
          csd.tex_index_in_array[i] = _shader_data.tex_index_in_array[i];
          csd.tex_animations[i] = _shader_data.tex_animations[i];
        }
      }
    }

    // normalize "bool" values
    csd.is_textured = _preview_data->texture_count ? 1 : 0;
    csd.has_shadow = _chunk_shadow ? 1 : 0;
  }
  else
  {
    texture_count = texture_set->num();

    if (texture_count)
    {
      if (!mt->use_no_alpha_alphamap())
      {
        update_alpha_shadow_map();
      }

      for (int i = 0; i < texture_count; ++i)
      {
        if (_texture_set_need_update)
        {
          std::pair<int, int> param = tileset_handler.get_texture_position(texture_set->texture(i));

          csd.tex_array_index[i] = param.first;
          csd.tex_index_in_array[i] = param.second;
          csd.tex_animations[i] = math::vector_4d(texture_set->anim_param(i), 0.f);
        }
        else // no change, reuse the old values
        {
          csd.tex_array_index[i] = _shader_data.tex_array_index[i];
          csd.tex_index_in_array[i] = _shader_data.tex_index_in_array[i];
          csd.tex_animations[i] = _shader_data.tex_animations[i];
        }
      }
    }

    // normalize "bool" values
    csd.is_textured = texture_count ? 1 : 0;
    csd.has_shadow = _chunk_shadow ? 1 : 0;
  }

  csd.is_copied = _is_copied ? 1 : 0;
  csd.is_in_paste_zone = _is_in_paste_zone ? 1 : 0;

  // todo: only check if the textures have changed on the chunk
  // or the current selected texture has changed
  if (selected_texture_changed || !_uploaded)
  {
    bool cant_paint = texture_count == 4 && !canPaintTexture(current_texture);

    csd.cant_paint = cant_paint ? 1 : 0;
  }
  else
  {
    csd.cant_paint = _shader_data.cant_paint;
  }

  csd.draw_impassible_flag = header.flags.flags.impass ? 1 : 0;
  csd.areaid_color = (math::vector_4d)area_id_colors[_area_id];

  gl.bufferSubData(GL_UNIFORM_BUFFER, (sizeof(chunk_shader_data) * (py * 16 + px)), sizeof(chunk_shader_data), &csd);

  _uploaded = true;
  _shader_data_need_update = false;
  _shader_data = csd;
}

void MapChunk::prepare_draw ( const math::vector_3d& camera
                            , bool need_visibility_update
                            , bool selected_texture_changed
                            , std::string const& current_texture
                            , std::map<int, misc::random_color>& area_id_colors
                            , display_mode display
                            , noggit::tileset_array_handler& tileset_handler
                            , std::vector<void*>& indices_offsets
                            , std::vector<int>& indices_count
                            )
{
  if (need_visibility_update || _need_lod_update)
  {
    int old_lod = _lod_level;
    _lod_level = get_lod_level(camera, display);
    _need_lod_update = false;

    if (old_lod != _lod_level)
    {
      indices_offsets[chunk_index()] = lod_indices_ptr(_lod_level);
      indices_count[chunk_index()] = _indices_count_per_lod_level[_lod_level];
    }
  }

  if(_shader_data_need_update || selected_texture_changed)
  {
    update_shader_data( selected_texture_changed
                      , current_texture
                      , area_id_colors
                      , tileset_handler
                      );
  }

  if (_need_indice_buffer_update)
  {
    int offset = 0;

    for (int i = 0; i < indice_buffer_count; ++i)
    {
      gl.bufferSubData(GL_ELEMENT_ARRAY_BUFFER, (indices_offset() + offset) * sizeof(chunk_indice), _indice_strips[i].size() * sizeof(chunk_indice), _indice_strips[i].data());
      offset += max_indices_per_lod_level[i];
    }

    // no longer needed at this point
    _indice_strips.clear();

    _need_indice_buffer_update = false;

    indices_offsets[chunk_index()] = lod_indices_ptr(_lod_level);
    indices_count[chunk_index()] = _indices_count_per_lod_level[_lod_level];
  }

  if (_need_vao_update)
  {
    gl.bufferSubData(GL_ARRAY_BUFFER, vertex_offset() * sizeof(chunk_vertex),  mapbufsize * sizeof(chunk_vertex), _preview_data ? _preview_data->vertices.data() : vertices.data());
    _need_vao_update = false;
  }
}

void* MapChunk::lod_indices_ptr(int lod) const
{
  int offset = 0;

  for (int i = 0; i < lod; ++i)
  {
    offset += max_indices_per_lod_level[i];
  }

  return static_cast<char*>(0) + (indices_offset() + offset) * sizeof(chunk_indice);
}

void MapChunk::intersect (math::ray const& ray, selection_result* results, bool ignore_terrain_holes)
{
  if (!ray.intersect_bounds (vmin, vmax))
  {
    return;
  }

  // regen indices when needed (only with fps camera so no need to keep them in memory all the time)
  if(!ignore_terrain_holes && _indice_strips.empty())
  {
    initStrip();
  }

  std::vector<chunk_indice> const& indices = ignore_terrain_holes ? strip_without_holes : _indice_strips[0];
  chunk_indice offset = ignore_terrain_holes ? 0 : vertex_offset();

  for (int i (0); i < indices.size(); i += 3)
  {
    if ( auto distance = ray.intersect_triangle ( vertices[indices[i + 0] - offset].position
                                                , vertices[indices[i + 1] - offset].position
                                                , vertices[indices[i + 2] - offset].position
                                                )
       )
    {
      results->emplace_back
        ( *distance
        , selected_chunk_type
            ( this
            , std::make_tuple ( indices[i + 0] - offset
                              , indices[i + 1] - offset
                              , indices[i + 2] - offset
                              )
            , ray.position (*distance)
            )
        );
    }
  }
}

void MapChunk::updateVerticesData()
{
  vmin.y = std::numeric_limits<float>::max();
  vmax.y = std::numeric_limits<float>::lowest();

  for (int i(0); i < mapbufsize; ++i)
  {
    if (_preview_data)
    {
      vmin.y = std::min(vmin.y, _preview_data->vertices[i].position.y);
      vmax.y = std::max(vmax.y, _preview_data->vertices[i].position.y);
    }

    vmin.y = std::min(vmin.y, vertices[i].position.y);
    vmax.y = std::max(vmax.y, vertices[i].position.y);
  }

  update_intersect_points();
  require_vertices_buffer_update();

  // update adt extents each time the min/max height of a chunk might have changed
  mt->chunk_height_changed();
}

void MapChunk::recalcNorms (std::function<std::optional<float> (float, float)> height)
{
  auto point
  (
    [&] (math::vector_3d& v, float xdiff, float zdiff)
    {
      return math::vector_3d
             ( v.x + xdiff
             , height (v.x + xdiff, v.z + zdiff).value_or(v.y)
             , v.z + zdiff
             );
    }
  );

  float const half_unit = UNITSIZE / 2.f;

  for (int i = 0; i<mapbufsize; ++i)
  {
    math::vector_3d const P1 (point(vertices[i].position, -half_unit, -half_unit));
    math::vector_3d const P2 (point(vertices[i].position,  half_unit, -half_unit));
    math::vector_3d const P3 (point(vertices[i].position,  half_unit,  half_unit));
    math::vector_3d const P4 (point(vertices[i].position, -half_unit,  half_unit));

    math::vector_3d const N1 ((P2 - vertices[i].position) % (P1 - vertices[i].position));
    math::vector_3d const N2 ((P3 - vertices[i].position) % (P2 - vertices[i].position));
    math::vector_3d const N3 ((P4 - vertices[i].position) % (P3 - vertices[i].position));
    math::vector_3d const N4 ((P1 - vertices[i].position) % (P4 - vertices[i].position));

    math::vector_3d Norm (N1 + N2 + N3 + N4);
    Norm.normalize();

    Norm.x = std::floor(Norm.x * 127) / 127;
    Norm.y = std::floor(Norm.y * 127) / 127;
    Norm.z = std::floor(Norm.z * 127) / 127;

    //! \todo: find out why recalculating normals without changing the terrain result in slightly different normals
    vertices[i].normal = {-Norm.z, Norm.y, -Norm.x};
  }

   require_vertices_buffer_update();

   mt->need_chunk_data_update();
}

bool MapChunk::changeTerrain(math::vector_3d const& pos, float change, float radius, int BrushType, float inner_radius, terrain_edit_mode edit_mode)
{
  float dist, xdiff, zdiff;
  bool changed = false;

  for (int i = 0; i < mapbufsize; ++i)
  {
    float height = vertices[i].position.y;

    if (edit_mode == terrain_edit_mode::only_below_cursor)
    {
      if ((change > 0.f && height > pos.y + 0.05) || (change < 0.f && height < pos.y - 0.05))
      {
        continue;
      }
    }
    else if (edit_mode == terrain_edit_mode::only_above_cursor)
    {
      if ((change > 0.f && height < pos.y + 0.05) || (change < 0.f && height > pos.y - 0.05))
      {
        continue;
      }
    }


    xdiff = vertices[i].position.x - pos.x;
    zdiff = vertices[i].position.z - pos.z;
    if (BrushType == eTerrainType_Quadra)
    {
      if ((std::abs(xdiff) < std::abs(radius / 2)) && (std::abs(zdiff) < std::abs(radius / 2)))
      {
        dist = std::sqrt(xdiff*xdiff + zdiff*zdiff);
        vertices[i].position.y += change * (1.0f - dist * inner_radius / radius);
        changed = true;
      }
    }
    else
    {
      dist = std::sqrt(xdiff*xdiff + zdiff*zdiff);
      if (dist < radius)
      {
        changed = true;

        switch (BrushType)
        {
          case eTerrainType_Flat:
            vertices[i].position.y += change;
            break;
          case eTerrainType_Linear:
            vertices[i].position.y += change * (1.0f - dist * (1.0f - inner_radius) / radius);
            break;
          case eTerrainType_Smooth:
            vertices[i].position.y += change / (1.0f + dist / radius);
            break;
          case eTerrainType_Polynom:
            vertices[i].position.y += change*((dist / radius)*(dist / radius) + dist / radius + 1.0f);
            break;
          case eTerrainType_Trigo:
            vertices[i].position.y += change*cos(dist / radius);
            break;
          case eTerrainType_Gaussian:
            vertices[i].position.y += dist < radius * inner_radius ? change * std::exp(-(std::pow(radius * inner_radius / radius, 2) / (2 * std::pow(0.39f, 2)))) : change * std::exp(-(std::pow(dist / radius, 2) / (2 * std::pow(0.39f, 2))));

            break;
          default:
            LogError << "Invalid terrain edit type (" << BrushType << ")" << std::endl;
            changed = false;
            break;
        }
      }
    }
  }
  if (changed)
  {
    updateVerticesData();
  }
  return changed;
}

void MapChunk::reset_mccv()
{
  _has_mccv = false;
  maybe_create_mccv();
  require_vertices_buffer_update();
  mt->need_chunk_data_update();
}

bool MapChunk::hasColors()
{
  return _has_mccv;
}

void MapChunk::maybe_create_mccv()
{
  if (!_has_mccv)
  {
    for (int i = 0; i < mapbufsize; ++i)
    {
      vertices[i].color = math::vector_3d(1.f, 1.f, 1.f);
    }

    _has_mccv = true;
  }
}

bool MapChunk::ChangeMCCV(math::vector_3d const& pos, math::vector_4d const& color, float change, float radius, bool editMode)
{
  float dist;
  bool changed = false;

  if (!_has_mccv)
  {
    for (int i = 0; i < mapbufsize; ++i)
    {
      vertices[i].color.x = 1.0f; // set default shaders
      vertices[i].color.y = 1.0f;
      vertices[i].color.z = 1.0f;
    }

    changed = true;
    header.flags.flags.has_mccv = 1;
    _has_mccv = true;
  }

  for (int i = 0; i < mapbufsize; ++i)
  {
    dist = misc::dist(vertices[i].position, pos);
    if (dist <= radius)
    {
      float edit = change * (1.0f - dist / radius);
      if (editMode)
      {
        vertices[i].color.x += (color.x / 0.5f - vertices[i].color.x) * edit;
        vertices[i].color.y += (color.y / 0.5f - vertices[i].color.y) * edit;
        vertices[i].color.z += (color.z / 0.5f - vertices[i].color.z) * edit;
      }
      else
      {
        vertices[i].color.x += (1.0f - vertices[i].color.x) * edit;
        vertices[i].color.y += (1.0f - vertices[i].color.y) * edit;
        vertices[i].color.z += (1.0f - vertices[i].color.z) * edit;
      }

      vertices[i].color.x = std::min(std::max(vertices[i].color.x, 0.0f), 2.0f);
      vertices[i].color.y = std::min(std::max(vertices[i].color.y, 0.0f), 2.0f);
      vertices[i].color.z = std::min(std::max(vertices[i].color.z, 0.0f), 2.0f);

      changed = true;
    }
  }

  require_vertices_buffer_update();

  mt->need_chunk_data_update();

  return changed;
}

math::vector_3d MapChunk::pickMCCV(math::vector_3d const& pos)
{
  float dist;
  float cur_dist = UNITSIZE;

  if (!_has_mccv)
  {
    return math::vector_3d(1.0f, 1.0f, 1.0f);
  }

  int v_index = 0;
  for (int i = 0; i < mapbufsize; ++i)
  {
    dist = misc::dist(vertices[i].position, pos);
    if (dist <= cur_dist)
    {
      cur_dist = dist;
      v_index = i;
    }
  }

  return vertices[v_index].color;

}

bool MapChunk::flattenTerrain ( math::vector_3d const& pos
                              , float remain
                              , float radius
                              , int BrushType
                              , flatten_mode const& mode
                              , math::vector_3d const& origin
                              , math::degrees angle
                              , math::degrees orientation
                              )
{
  bool changed (false);

  if (BrushType == eFlattenType_Smooth_Inner)
  {
    return smooth_inner_vertices(pos, remain, radius);
  }

  for (int i(0); i < mapbufsize; ++i)
  {
	  float const dist(misc::dist(vertices[i].position, pos));

	  if (dist >= radius)
	  {
		  continue;
	  }

	  float const ah(origin.y
		  + ((vertices[i].position.x - origin.x) * math::cos(orientation)
			  + (vertices[i].position.z - origin.z) * math::sin(orientation)
			  ) * math::tan(angle)
	  );

	  if ((!mode.lower && ah < vertices[i].position.y)
		  || (!mode.raise && ah > vertices[i].position.y)
		  )
	  {
		  continue;
	  }

	  if (BrushType == eFlattenType_Origin)
	  {
		  vertices[i].position.y = origin.y;
	  }
    else
    {
      vertices[i].position.y = math::interpolation::linear
        ( BrushType == eFlattenType_Flat ? remain
        : BrushType == eFlattenType_Linear ? remain * (1.f - dist / radius)
        : BrushType == eFlattenType_Smooth ? pow (remain, 1.f + dist / radius)
        : throw std::logic_error ("bad brush type")
        , vertices[i].position.y
        , ah
        );
    }

    changed = true;
  }

  if (changed)
  {
    updateVerticesData();
  }

  return changed;
}

bool MapChunk::blurTerrain ( math::vector_3d const& pos
                           , float remain
                           , float radius
                           , int BrushType
                           , flatten_mode const& mode
                           , std::function<std::optional<float> (float, float)> height
                           )
{
  bool changed (false);

  if (BrushType == eFlattenType_Origin || BrushType == eFlattenType_Smooth_Inner)
  {
    return false;
  }

  for (int i (0); i < mapbufsize; ++i)
  {
    float const dist(misc::dist(vertices[i].position, pos));

    if (dist >= radius)
    {
      continue;
    }

    int Rad = (int)(radius / UNITSIZE);
    float TotalHeight = 0;
    float TotalWeight = 0;
    for (int j = -Rad * 2; j <= Rad * 2; ++j)
    {
      float tz = pos.z + j * UNITSIZE / 2;
      for (int k = -Rad; k <= Rad; ++k)
      {
        float tx = pos.x + k*UNITSIZE + (j % 2) * UNITSIZE / 2.0f;
        float dist2 = misc::dist (tx, tz, vertices[i].position.x, vertices[i].position.z);
        if (dist2 > radius)
          continue;
        auto h (height (tx, tz));
        if (h)
        {
          TotalHeight += (1.0f - dist2 / radius) * h.value();
          TotalWeight += (1.0f - dist2 / radius);
        }
      }
    }

    float target = TotalHeight / TotalWeight;
    float& y = vertices[i].position.y;

    if ((target > y && !mode.raise) || (target < y && !mode.lower))
    {
      continue;
    }

    y = math::interpolation::linear
      ( BrushType == eFlattenType_Flat ? remain
      : BrushType == eFlattenType_Linear ? remain * (1.f - dist / radius)
      : BrushType == eFlattenType_Smooth ? pow (remain, 1.f + dist / radius)
      : throw std::logic_error ("bad brush type")
      , y
      , target
      );

    changed = true;
  }

  if (changed)
  {
    updateVerticesData();
  }

  return changed;
}

bool MapChunk::smooth_inner_vertices(math::vector_3d const& pos, float remain, float radius)
{
  bool changed = false;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      int id = (z * 17 + 9) + x;

      math::vector_3d& v_pos = vertices[id].position;
      float const dist(misc::dist(v_pos, pos));

      if (dist < radius)
      {
        math::vector_3d const& v00 = vertices[id-9].position;
        math::vector_3d const& v10 = vertices[id-8].position;
        math::vector_3d const& v11 = vertices[id+9].position;
        math::vector_3d const& v01 = vertices[id+8].position;

        float dst_0 = (v00 - v11).length_squared();
        float dst_1 = (v01 - v10).length_squared();

        float target;

        // target the center point between the 2 closest diagonally opposed corners
        // todo: can be improved (hopefully)
        if (dst_0 < dst_1)
        {
          target = ((v00 + v11) * 0.5f).y;
        }
        else
        {
          target = ((v01 + v10) * 0.5f).y;
        }

        float diff = target - v_pos.y;

        v_pos.y += diff * remain;

        changed = true;
      }
    }
  }

  if (changed)
  {
    updateVerticesData();
  }

  return changed;
}


void MapChunk::eraseTextures()
{
  texture_set_changed();
  texture_set->eraseTextures();
}

void MapChunk::remove_texture_duplicates()
{
  texture_set_changed();
  texture_set->removeDuplicate();
}

void MapChunk::remove_unused_textures(float threshold)
{
  texture_set_changed();
  texture_set->eraseUnusedTextures(threshold);
}

void MapChunk::change_texture_flag(scoped_blp_texture_reference const& tex, std::size_t flag, bool add)
{
  texture_set_changed();
  texture_set->change_texture_flag(tex, flag, add);
}

void MapChunk::clear_texture_flags()
{
  texture_set_changed();
  texture_set->clear_texture_flags();
}

int MapChunk::addTexture(scoped_blp_texture_reference texture)
{
  texture_set_changed();
  return texture_set->addTexture(std::move (texture));
}

void MapChunk::switchTexture(scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture)
{
  texture_set_changed();
  texture_set->replace_texture(oldTexture, std::move (newTexture));
}

bool MapChunk::paintTexture(math::vector_3d const& pos, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture)
{
  texture_set_changed();
  return texture_set->paintTexture(xbase, zbase, pos.x, pos.z, brush, strength, pressure, std::move (texture));
}

bool MapChunk::replaceTexture(math::vector_3d const& pos, Brush const& brush, float change, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture)
{
  texture_set_changed();
  return texture_set->replace_texture(xbase, zbase, pos.x, pos.z, brush, change, old_texture, std::move (new_texture));
}

bool MapChunk::canPaintTexture(std::string const& texture)
{
  return texture_set->canPaintTexture(texture);
}

void MapChunk::clear_shadows()
{
  require_shader_data_update();
  _chunk_shadow.reset();
}

void MapChunk::set_shadow(math::vector_3d const& pos, float radius, bool add)
{
  if (misc::getShortestDist(pos.x, pos.z, xbase, zbase, CHUNKSIZE) > radius)
  {
    return;
  }

  if (!_chunk_shadow)
  {
    _chunk_shadow = std::make_unique<chunk_shadow>();
  }

  math::vector_3d p(xbase, 0.f, zbase);

  for (int ix = 0; ix < 64; ++ix)
  {
    p.z = zbase;

    for (int iz = 0; iz < 64; ++iz)
    {
      if (misc::getShortestDist(pos, p, TEXDETAILSIZE) <= radius)
      {
        std::uint64_t& value = _chunk_shadow.get()->data[iz];
        std::uint64_t flag = std::uint64_t(1) << ix;

        if (add)
        {
          value |= flag;
        }
        else
        {
          value &= ~flag;
        }
      }

      p.z += TEXDETAILSIZE;
    }

    p.x += TEXDETAILSIZE;
  }

  texture_set_changed();
  texture_set->require_update(); // shadows are tied with the alphamap
}

void MapChunk::set_shadows(std::vector<std::uint8_t> const& tile_shadow_map, int threshold)
{
  _chunk_shadow = std::make_unique<chunk_shadow>();

  int start_x = px * 64;
  int start_z = py * 64;

  for (int ix = 0; ix < 64; ++ix)
  {
    for (int iz = 0; iz < 64; ++iz)
    {
      // the data is flipped on the z axis
      int id = (1023 - (start_z + iz)) * 1024 + start_x + ix;

      if (tile_shadow_map[id] >= threshold)
      {
        std::uint64_t& value = _chunk_shadow.get()->data[iz];
        std::uint64_t flag = std::uint64_t(1) << ix;
        value |= flag;
      }
    }
  }

  texture_set_changed();
  texture_set->require_update(); // shadows are tied with the alphamap
}

bool MapChunk::isHole(int i, int j) const
{
  if (_preview_data)
  {
    return (_preview_data->holes & ((1 << ((j * 4) + i)))) != 0;
  }
  else
  {
    return (_4x4_holes & ((1 << ((j * 4) + i)))) != 0;
  }
}

void MapChunk::setHole(math::vector_3d const& pos, bool big, bool add)
{
  if (big)
  {
    _4x4_holes = add ? 0xFFFFFFFF : 0x0;
  }
  else
  {
    int v = 1 << ((int)((pos.z - zbase) / MINICHUNKSIZE) * 4 + (int)((pos.x - xbase) / MINICHUNKSIZE));
    _4x4_holes = add ? (_4x4_holes | v) : (_4x4_holes & ~v);
  }

  initStrip();
}

void MapChunk::setAreaID(int ID)
{
  _area_id = ID;

  require_shader_data_update();
}

int MapChunk::getAreaID()
{
  return _area_id;
}


void MapChunk::setFlag(bool changeto, uint32_t flag)
{
  if (changeto)
  {
    header.flags.value |= flag;
  }
  else
  {
    header.flags.value &= ~flag;
  }

  require_shader_data_update();
}

void MapChunk::save( util::sExtendableArray &lADTFile
                   , int &lCurrentPosition
                   , int &lMCIN_Position
                   , std::map<std::string, int> &lTextures
                   , std::vector<WMOInstance> &lObjectInstances
                   , std::vector<ModelInstance>& lModelInstances
                   )
{
  int lID;
  int lMCNK_Size = 0x80;
  int lMCNK_Position = lCurrentPosition;
  lADTFile.Extend(8 + 0x80);  // This is only the size of the header. More chunks will increase the size.
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCNK', lMCNK_Size);
  lADTFile.GetPointer<MCIN>(lMCIN_Position + 8)->mEntries[py * 16 + px].offset = lCurrentPosition; // check this

                                                                                                   // MCNK data
  lADTFile.Insert(lCurrentPosition + 8, 0x80, reinterpret_cast<char*>(&(header)));
  auto const lMCNK_header = lADTFile.GetPointer<MapChunkHeader>(lCurrentPosition + 8);

  // the loaded flags are kept verbatim: vanilla (1.12.1) files do set some
  // of the wotlk-named bits (e.g. do_not_fix_alpha_map), clearing them
  // would rewrite chunks that were not edited
  lMCNK_header->flags = header.flags;
  lMCNK_header->holes = _4x4_holes;
  lMCNK_header->areaid = _area_id;

  lMCNK_header->nLayers = -1;
  lMCNK_header->nDoodadRefs = -1;
  lMCNK_header->ofsHeight = -1;
  lMCNK_header->ofsNormal = -1;
  lMCNK_header->ofsLayer = -1;
  lMCNK_header->ofsRefs = -1;
  lMCNK_header->ofsAlpha = -1;
  lMCNK_header->sizeAlpha = -1;
  lMCNK_header->ofsShadow = -1;
  lMCNK_header->sizeShadow = -1;
  lMCNK_header->nMapObjRefs = -1;
  lMCNK_header->ofsMCCV = -1;

  //! \todo  Implement sound emitter support. Or not.
  lMCNK_header->ofsSndEmitters = 0;
  lMCNK_header->nSndEmitters = 0;

  lMCNK_header->ofsLiquid = 0;
  //! \todo Is this still 8 if no chunk is present? Or did they correct that?
  lMCNK_header->sizeLiquid = 8;

  lMCNK_header->ypos = vertices[0].position.y;

  memset(lMCNK_header->low_quality_texture_map, 0, 0x10);

  std::vector<uint8_t> lod_texture_map = texture_set->lod_texture_map();

  for (int i = 0; i < lod_texture_map.size(); ++i)
  {
    const size_t array_index(i / 4);
    // it's a uint2 array so we need to write the uint2 in the order they will be on disk,
    // this means writing to the highest bits of the uint8 first
    const size_t bit_index((3 - ((i) % 4)) * 2);

    lMCNK_header->low_quality_texture_map[array_index] |= ((lod_texture_map[i] & 3) << bit_index);
  }

  lCurrentPosition += 8 + 0x80;

  // MCVT
  int lMCVT_Size = mapbufsize * 4;

  lADTFile.Extend(8 + lMCVT_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCVT', lMCVT_Size);

  auto header_ptr = lADTFile.GetPointer<MapChunkHeader>(lMCNK_Position + 8);

  header_ptr->ofsHeight = lCurrentPosition - lMCNK_Position;

  auto const lHeightmap = lADTFile.GetPointer<float>(lCurrentPosition + 8);

  for (int i = 0; i < mapbufsize; ++i)
    lHeightmap[i] = vertices[i].position.y - vertices[0].position.y;

  lCurrentPosition += 8 + lMCVT_Size;
  lMCNK_Size += 8 + lMCVT_Size;

  // MCCV
  int lMCCV_Size = 0;
  if (_has_mccv)
  {
    lMCCV_Size = mapbufsize * sizeof(unsigned int);
    lADTFile.Extend(8 + lMCCV_Size);
    SetChunkHeader(lADTFile, lCurrentPosition, 'MCCV', lMCCV_Size);
    header_ptr->ofsMCCV = lCurrentPosition - lMCNK_Position;

    auto const lmccv = lADTFile.GetPointer<unsigned int>(lCurrentPosition + 8);

    for (int i = 0; i < mapbufsize; ++i)
    {
      lmccv[i] = (((unsigned char)(vertices[i].color.z * 127.0f) & 0xFF) << 0)
               + (((unsigned char)(vertices[i].color.y * 127.0f) & 0xFF) <<  8)
               + (((unsigned char)(vertices[i].color.x * 127.0f) & 0xFF) << 16);
    }

    lCurrentPosition += 8 + lMCCV_Size;
    lMCNK_Size += 8 + lMCCV_Size;
  }
  else
  {
    header_ptr->ofsMCCV = 0;
  }

  // MCNR
  int lMCNR_Size = mapbufsize * 3;

  lADTFile.Extend(8 + lMCNR_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCNR', lMCNR_Size);

  header_ptr->ofsNormal = lCurrentPosition - lMCNK_Position;

  auto const lNormals = lADTFile.GetPointer<char>(lCurrentPosition + 8);

  for (int i = 0; i < mapbufsize; ++i)
  {
    lNormals[i * 3 + 0] = static_cast<char>(vertices[i].normal.x * 127);
    lNormals[i * 3 + 1] = static_cast<char>(vertices[i].normal.z * 127);
    lNormals[i * 3 + 2] = static_cast<char>(vertices[i].normal.y * 127);
  }

  lCurrentPosition += 8 + lMCNR_Size;
  lMCNK_Size += 8 + lMCNR_Size;
  //        }

  // Unknown MCNR bytes
  // These are not in as we have data or something but just to make the files more blizzlike.
  //        {
  lADTFile.Extend(13);
  lCurrentPosition += 13;
  lMCNK_Size += 13;
  //        }

  // MCLY
  //        {
  size_t lMCLY_Size = texture_set->num() * 0x10;

  lADTFile.Extend(8 + lMCLY_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCLY', lMCLY_Size);

  header_ptr->ofsLayer = lCurrentPosition - lMCNK_Position;
  header_ptr->nLayers = texture_set->num();

  std::vector<std::vector<uint8_t>> alphamaps = texture_set->save_alpha(use_big_alphamap);
  int lMCAL_Size = 0;

  // MCLY data
  for (size_t j = 0; j < texture_set->num(); ++j)
  {
    auto const lLayer = lADTFile.GetPointer<ENTRY_MCLY>(lCurrentPosition + 8 + 0x10 * j);

    lLayer->textureID = lTextures.find(texture_set->texture(j))->second;
    lLayer->flags = texture_set->flag(j);
    lLayer->ofsAlpha = lMCAL_Size;
    lLayer->effectID = texture_set->effect(j);

    if (j == 0)
    {
      lLayer->flags &= ~(FLAG_USE_ALPHA | FLAG_ALPHA_COMPRESSED);
    }
    else
    {
      lLayer->flags |= FLAG_USE_ALPHA;
      //! \todo find out why compression fuck up textures ingame
      lLayer->flags &= ~FLAG_ALPHA_COMPRESSED;

      lMCAL_Size += alphamaps[j - 1].size();
    }
  }

  lCurrentPosition += 8 + lMCLY_Size;
  lMCNK_Size += 8 + lMCLY_Size;
  //        }

  // MCRF
  //        {
  std::list<int> lDoodadIDs;
  std::list<int> lObjectIDs;

  math::vector_3d lChunkExtents[2];
  lChunkExtents[0] = math::vector_3d(xbase, 0.0f, zbase);
  lChunkExtents[1] = math::vector_3d(xbase + CHUNKSIZE, 0.0f, zbase + CHUNKSIZE);

  // search all wmos that are inside this chunk
  lID = 0;
  for(auto const& wmo : lObjectInstances)
  {
    if (wmo.isInsideRect(lChunkExtents))
    {
      lObjectIDs.push_back(lID);
    }

    lID++;
  }

  // search all models that are inside this chunk
  lID = 0;
  for(auto const& model : lModelInstances)
  {
    if (model.isInsideRect (lChunkExtents))
    {
      lDoodadIDs.push_back(lID);
    }
    lID++;
  }

  int lMCRF_Size = 4 * (lDoodadIDs.size() + lObjectIDs.size());
  lADTFile.Extend(8 + lMCRF_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCRF', lMCRF_Size);

  header_ptr->ofsRefs = lCurrentPosition - lMCNK_Position;
  header_ptr->nDoodadRefs = lDoodadIDs.size();
  header_ptr->nMapObjRefs = lObjectIDs.size();

  // MCRF data
  auto const lReferences = lADTFile.GetPointer<int>(lCurrentPosition + 8);

  lID = 0;
  for (std::list<int>::iterator it = lDoodadIDs.begin(); it != lDoodadIDs.end(); ++it)
  {
    lReferences[lID] = *it;
    lID++;
  }

  for (std::list<int>::iterator it = lObjectIDs.begin(); it != lObjectIDs.end(); ++it)
  {
    lReferences[lID] = *it;
    lID++;
  }

  lCurrentPosition += 8 + lMCRF_Size;
  lMCNK_Size += 8 + lMCRF_Size;
  //        }

  // MCSH
  if (!shadow_map_is_empty())
  {
    header.flags.flags.has_mcsh = 1;

    int lMCSH_Size = 0x200;
    lADTFile.Extend(8 + lMCSH_Size);
    SetChunkHeader(lADTFile, lCurrentPosition, 'MCSH', lMCSH_Size);

    header_ptr->ofsShadow = lCurrentPosition - lMCNK_Position;
    header_ptr->sizeShadow = 0x200;

    auto const lLayer = lADTFile.GetPointer<char>(lCurrentPosition + 8);

    memcpy(lLayer.get(), _chunk_shadow->data.data(), 0x200);

    lCurrentPosition += 8 + lMCSH_Size;
    lMCNK_Size += 8 + lMCSH_Size;
  }
  else
  {
    header.flags.flags.has_mcsh = 0;
    header_ptr->ofsShadow = 0;
    header_ptr->sizeShadow = 0;
  }

  // MCAL
  lADTFile.Extend(8 + lMCAL_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCAL', lMCAL_Size);

  header_ptr->ofsAlpha = lCurrentPosition - lMCNK_Position;
  header_ptr->sizeAlpha = 8 + lMCAL_Size;

  auto lAlphaMaps = lADTFile.GetPointer<char>(lCurrentPosition + 8);

  for (auto alpha : alphamaps)
  {
    memcpy(lAlphaMaps.get(), alpha.data(), alpha.size());
    lAlphaMaps += alpha.size();
  }

  lCurrentPosition += 8 + lMCAL_Size;
  lMCNK_Size += 8 + lMCAL_Size;

  // vanilla MCLQ liquid: always written, an empty chunk is written when
  // the chunk has no liquid (vanilla adts always contain the chunk)
  {
    auto liquids = liquid_chunk();

    if (liquids && liquids->displayed_layer_count() > 0)
    {
      int liquids_size = 8 + liquids->displayed_layer_count() * sizeof(mclq);

      lMCNK_Size += liquids_size;

      header_ptr->sizeLiquid = liquids_size;
      header_ptr->ofsLiquid = lCurrentPosition - lMCNK_Position;

      // current position updated inside
      liquids->save_mclq(lADTFile, lMCNK_Position, lCurrentPosition);
    }
    // no liquid, vanilla adt still have an empty chunk
    else
    {
      lADTFile.Extend(8);
      // size seems to be 0 in vanilla adts in the mclq chunk's header and set right in the mcnk header (layer_size * n_layer + 8)
      SetChunkHeader(lADTFile, lCurrentPosition, 'MCLQ', 0);

      header_ptr->sizeLiquid = 8;
      header_ptr->ofsLiquid = lCurrentPosition - lMCNK_Position;

      // clear MCLQ liquid flags (0x4, 0x8, 0x10, 0x20)
      header_ptr->flags.value &= 0xFFFFFFC3;

      lCurrentPosition += 8;
      lMCNK_Size += 8;
    }
  }

  // MCSE
  int lMCSE_Size = 0;
  lADTFile.Extend(8 + lMCSE_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCSE', lMCSE_Size);

  header_ptr->ofsSndEmitters = lCurrentPosition - lMCNK_Position;
  header_ptr->nSndEmitters = lMCSE_Size / 0x1C;

  lCurrentPosition += 8 + lMCSE_Size;
  lMCNK_Size += 8 + lMCSE_Size;

  lADTFile.GetPointer<sChunkHeader>(lMCNK_Position)->mSize = lMCNK_Size;
  lADTFile.GetPointer<MCIN>(lMCIN_Position + 8)->mEntries[py * 16 + px].size = lMCNK_Size + sizeof (sChunkHeader);
}


bool MapChunk::fixGapLeft(const MapChunk* chunk)
{
  if (!chunk)
    return false;

  bool changed = false;

  for (size_t i = 0; i <= 136; i+= 17)
  {
    float h = chunk->vertices[i + 8].position.y;
    if (vertices[i].position.y != h)
    {
      vertices[i].position.y = h;
      changed = true;
    }
  }

  if (changed)
  {
    updateVerticesData();
  }

  return changed;
}

bool MapChunk::fixGapAbove(const MapChunk* chunk)
{
  if (!chunk)
    return false;

  bool changed = false;

  for (size_t i = 0; i < 9; i++)
  {
    float h = chunk->vertices[i + 136].position.y;
    if (vertices[i].position.y != h)
    {
      vertices[i].position.y = h;
      changed = true;
    }
  }

  if (changed)
  {
    updateVerticesData();
  }

  return changed;
}


void MapChunk::selectVertex(math::vector_3d const& pos, float radius, std::set<math::vector_3d*>& selected_vertices)
{
  if (misc::getShortestDist(pos.x, pos.z, xbase, zbase, CHUNKSIZE) > radius)
  {
    return;
  }

  for (int i = 0; i < mapbufsize; ++i)
  {
    if (misc::dist(pos.x, pos.z, vertices[i].position.x, vertices[i].position.z) <= radius)
    {
      selected_vertices.emplace(&vertices[i].position);
    }
  }
}

void MapChunk::selectVertex(math::vector_3d const& pos1, math::vector_3d const& pos2, std::set<math::vector_3d*>& selected_vertices)
{
  for(int i = 0; i< mapbufsize; ++i)
  {
    if(
      pos1.x<=vertices[i].position.x && pos2.x>=vertices[i].position.x &&
      pos1.z<=vertices[i].position.z && pos2.z>=vertices[i].position.z
    )
    {
      selected_vertices.emplace(&vertices[i].position);
    }
  }
}

void MapChunk::fixVertices(std::set<math::vector_3d*>& selected)
{
  std::vector<int> ids ={ 0, 1, 17, 18 };
  // iterate through each "square" of vertices
  for (int i = 0; i < 64; ++i)
  {
    int not_selected = 0, count = 0, mid_vertex = ids[0] + 9;
    float h = 0.0f;

    for (int& index : ids)
    {
      if (selected.find(&vertices[index].position) == selected.end())
      {
        not_selected = index;
      }
      else
      {
        count++;
      }
      h += vertices[index].position.y;
      index += (((i+1) % 8) == 0) ? 10 : 1;
    }

    if (count == 2)
    {
      vertices[mid_vertex].position.y = h * 0.25f;
    }
    else if (count == 3)
    {
      vertices[mid_vertex].position.y = (h - vertices[not_selected].position.y) / 3.0f;
    }
  }
}

bool MapChunk::isBorderChunk(std::set<math::vector_3d*>& selected)
{
  for (int i = 0; i < mapbufsize; ++i)
  {
    // border chunk if at least a vertex isn't selected
    if (selected.find(&vertices[i].position) == selected.end())
    {
      return true;
    }
  }

  return false;
}

liquid_chunk* MapChunk::liquid_chunk() const
{
  return mt->Water.getChunk(px, py);
}

void MapChunk::update_alpha_shadow_map()
{
  if (_preview_data)
  {
    chunk_shadow* shadow = nullptr;

    if (!_preview_params->clear_shadows)
    {
      if (_preview_params->shadows)
      {
        if (_preview_data->shadows)
        {
          shadow = &_preview_data->shadows.value();
        }
      }
      else if (_chunk_shadow)
      {
        shadow = _chunk_shadow.get();
      }
    }

    texture_set->update_alpha_shadow_map_if_needed(px, py, shadow, _preview_params->alphamaps ? _preview_data.get() : nullptr);
  }
  else
  {
    texture_set->update_alpha_shadow_map_if_needed(px, py, _chunk_shadow ? _chunk_shadow.get() : nullptr, nullptr);
  }
}
