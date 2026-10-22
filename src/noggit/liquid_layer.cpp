// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <noggit/Selection.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
  // 1.12.1 MCLQ water carries no texture coordinates, so they are derived from
  // the vertex index. Upstream sampled them as (index / 4), i.e. a texture
  // repeat every two chunk tiles; the wmo liquid path (wmo_liquid.cpp) uses the
  // raw tile index instead, one repeat per tile at the same 4.17 unit vertex
  // spacing. The finer mapping matches that convention: the coarse one
  // stretched a single texture across four tiles and read as a flat,
  // featureless sheet at distance.
  inline math::vector_2d default_uv(int px, int pz)
  {
    return {static_cast<float>(px), static_cast<float>(pz)};
  }
}

liquid_layer::liquid_layer(math::vector_3d const& base, float height, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(0)
  , _minimum(height)
  , _maximum(height)
  , _subchunks(0)
  , pos(base)
{
  create_vertices(height);

  changeLiquidID(_liquid_id);
  update_min_max();
}

liquid_layer::liquid_layer(math::vector_3d const& base, mclq& liquid, int liquid_id)
  : _liquid_id(liquid_id)
  , _minimum(liquid.min_height)
  , _maximum(liquid.max_height)
  , _subchunks(0)
  , pos(base)
{
  changeLiquidID(_liquid_id);

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      mclq_tile const& tile = liquid.tiles[z * 8 + x];

      bool const has_liquid (!tile.dont_render);

      misc::set_bit(_subchunks, x, z, has_liquid);

      // the type of the last visible tile wins, like in
      // liquid_chunk::from_mclq; keeping it verbatim is the only way the
      // mclq tile type survives the liquid_id round trip
      if (has_liquid)
      {
        _mclq_liquid_type = tile.liquid_type;
      }

      // preserve the per tile bits the editor does not model
      misc::set_bit(_mclq_covered, x, z, has_liquid);
      misc::set_bit(_mclq_fishable, x, z, tile.fishable);
      misc::set_bit(_mclq_fatigue, x, z, tile.fatigue);
      misc::set_bit(_mclq_flag_0x10, x, z, tile.flag_0x10);
      misc::set_bit(_mclq_flag_0x20, x, z, tile.flag_0x20);
    }
  }

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      mclq_vertex const& v = liquid.vertices[z * 9 + x];

      liquid_vertex lv;

      if (_liquid_vertex_format == 1)
      {
        lv.depth = 1.f;
        lv.uv = { static_cast<float>(v.magma.x) / 255.f, static_cast<float>(v.magma.y) / 255.f };
      }
      else
      {
        lv.depth = static_cast<float>(v.water.depth) / 255.f;
        lv.uv = default_uv(x, z);

        // depth bytes only exist in the water layout of the union
        _mclq_depth[z * 9 + x] = v.water.depth;
      }

      // keep the raw height: vanilla files use FLT_MAX as "no liquid here"
      // sentinel on dry vertices, and clamping it into the header range
      // turns it into a plausible real height; on reload such a height can
      // fall below the terrain and zero depths that were still intact on
      // the first load, which makes every save differ from the previous one
      lv.position = { pos.x + UNITSIZE * x, v.height, pos.z + UNITSIZE * z };

      _vertices.push_back(lv);
    }
  }

  for (int i = 0; i < 9 * 9; ++i)
  {
    _mclq_flow_0[i] = liquid.vertices[i].water.flow_0_pct;
    _mclq_flow_1[i] = liquid.vertices[i].water.flow_1_pct;
  }

  _mclq_n_flowvs = liquid.n_flowvs;
  _mclq_flowvs[0] = liquid.flowvs[0];
  _mclq_flowvs[1] = liquid.flowvs[1];

  _from_mclq = true;

  // do not recompute the heights: vanilla's stored min/max do not always
  // match a recomputation from the covered vertices, and an untouched
  // chunk must save back byte-identically; edits go through update_min_max
  _center = math::vector_3d(pos.x + CHUNKSIZE * 0.5f, (_maximum + _minimum) * 0.5f, pos.z + CHUNKSIZE * 0.5f);
  _fatigue_enabled = check_fatigue();
}

liquid_layer::liquid_layer(MPQFile &f, std::size_t base_pos, math::vector_3d const& base, MH2O_Information const& info, std::uint64_t infomask)
  : _liquid_id(info.liquid_id)
  , _liquid_vertex_format(info.liquid_vertex_format)
  , _minimum(info.minHeight)
  , _maximum(info.maxHeight)
  , _subchunks(0)
  , pos(base)
{
  if (info.width > 8 || info.height > 8 || info.xOffset > 8 || info.yOffset > 8
    || info.xOffset + info.width > 8 || info.yOffset + info.height > 8)
  {
    throw std::out_of_range ("liquid_layer: info extents out of range.");
  }
  if (_liquid_vertex_format > 2)
  {
    throw std::runtime_error ("liquid_layer: unsupported vertex format.");
  }

  int offset = 0;
  for (int z = 0; z < info.height; ++z)
  {
    for (int x = 0; x < info.width; ++x)
    {
      setSubchunk(x + info.xOffset, z + info.yOffset, (infomask >> offset) & 1);
      offset++;
    }
  }

  create_vertices(_minimum);

  if (info.ofsHeightMap)
  {
    f.seek(base_pos + info.ofsHeightMap);

    if (_liquid_vertex_format == 0 || _liquid_vertex_format == 1)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          float h;
          if (f.read(&h, sizeof(float)) != sizeof(float))
          {
            throw std::runtime_error ("liquid_layer: truncated height map.");
          }

          _vertices[z * 9 + x].position.y = std::clamp(h, _minimum, _maximum);
        }
      }
    }

    if (_liquid_vertex_format == 1)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          mh2o_uv uv;
          if (f.read(&uv, sizeof(mh2o_uv)) != sizeof(mh2o_uv))
          {
            throw std::runtime_error ("liquid_layer: truncated uv map.");
          }

          _vertices[z * 9 + x].uv =
            { static_cast<float>(uv.x) / 255.f
            , static_cast<float>(uv.y) / 255.f
            };
        }
      }
    }

    if (_liquid_vertex_format == 0 || _liquid_vertex_format == 2)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          std::uint8_t depth;
          if (f.read(&depth, sizeof(std::uint8_t)) != sizeof(std::uint8_t))
          {
            throw std::runtime_error ("liquid_layer: truncated depth map.");
          }
          _vertices[z * 9 + x].depth = static_cast<float>(depth) / 255.f;
        }
      }
    }
  }

  changeLiquidID(_liquid_id); // to update the liquid type
  update_min_max();
}

liquid_layer::liquid_layer(math::vector_3d const& base, noggit::liquid_layer_data const& data)
  : _subchunks(data.subchunk_mask)
  , _vertices(data.vertices)
  , pos(base)
{
  changeLiquidID(data.liquid_id);
  update_min_max();
}

liquid_layer::liquid_layer(liquid_layer&& other)
  : _fatigue_enabled(other._fatigue_enabled)
  , _need_data_update(other._need_data_update)
  , _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _center(other._center)
  , _subchunks(other._subchunks)
  , _from_mclq(other._from_mclq)
  , _mclq_covered(other._mclq_covered)
  , _mclq_fishable(other._mclq_fishable)
  , _mclq_fatigue(other._mclq_fatigue)
  , _mclq_flag_0x10(other._mclq_flag_0x10)
  , _mclq_flag_0x20(other._mclq_flag_0x20)
  , _mclq_flow_0(other._mclq_flow_0)
  , _mclq_flow_1(other._mclq_flow_1)
  , _mclq_depth(other._mclq_depth)
  , _render_zeroed_depth(other._render_zeroed_depth)
  , _mclq_n_flowvs(other._mclq_n_flowvs)
  , _mclq_flowvs(other._mclq_flowvs)
  , _vertices(other._vertices)
  , _indices_by_lod(other._indices_by_lod)
  , _indices_count_by_lod(other._indices_count_by_lod)
  , pos(other.pos)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer::liquid_layer(liquid_layer const& other)
  : _fatigue_enabled(other._fatigue_enabled)
  , _need_data_update(other._need_data_update)
  , _liquid_id(other._liquid_id)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _center(other._center)
  , _subchunks(other._subchunks)
  , _from_mclq(other._from_mclq)
  , _mclq_covered(other._mclq_covered)
  , _mclq_fishable(other._mclq_fishable)
  , _mclq_fatigue(other._mclq_fatigue)
  , _mclq_flag_0x10(other._mclq_flag_0x10)
  , _mclq_flag_0x20(other._mclq_flag_0x20)
  , _mclq_flow_0(other._mclq_flow_0)
  , _mclq_flow_1(other._mclq_flow_1)
  , _mclq_depth(other._mclq_depth)
  , _render_zeroed_depth(other._render_zeroed_depth)
  , _mclq_n_flowvs(other._mclq_n_flowvs)
  , _mclq_flowvs(other._mclq_flowvs)
  , _vertices(other._vertices)
  , _indices_by_lod(other._indices_by_lod)
  , _indices_count_by_lod(other._indices_count_by_lod)
  , pos(other.pos)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer& liquid_layer::operator= (liquid_layer&& other)
{
  std::swap(_liquid_id, other._liquid_id);
  std::swap(_minimum, other._minimum);
  std::swap(_maximum, other._maximum);
  std::swap(_center, other._center);
  std::swap(_subchunks, other._subchunks);
  std::swap(_from_mclq, other._from_mclq);
  std::swap(_mclq_covered, other._mclq_covered);
  std::swap(_mclq_fishable, other._mclq_fishable);
  std::swap(_mclq_fatigue, other._mclq_fatigue);
  std::swap(_mclq_flag_0x10, other._mclq_flag_0x10);
  std::swap(_mclq_flag_0x20, other._mclq_flag_0x20);
  std::swap(_mclq_flow_0, other._mclq_flow_0);
  std::swap(_mclq_flow_1, other._mclq_flow_1);
  std::swap(_mclq_depth, other._mclq_depth);
  std::swap(_render_zeroed_depth, other._render_zeroed_depth);
  std::swap(_mclq_n_flowvs, other._mclq_n_flowvs);
  std::swap(_mclq_flowvs, other._mclq_flowvs);
  std::swap(_vertices, other._vertices);
  std::swap(_indices_by_lod, other._indices_by_lod);
  std::swap(_indices_count_by_lod, other._indices_count_by_lod);
  std::swap(_fatigue_enabled, other._fatigue_enabled);
  std::swap(_need_data_update, other._need_data_update);
  std::swap(pos, other.pos);

  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
  other.changeLiquidID(other._liquid_id);

  return *this;
}

liquid_layer& liquid_layer::operator=(liquid_layer const& other)
{
  _liquid_id = other._liquid_id;
  _liquid_vertex_format = other._liquid_vertex_format;
  _minimum = other._minimum;
  _maximum = other._maximum;
  _center = other._center;
  _subchunks = other._subchunks;
  _from_mclq = other._from_mclq;
  _mclq_covered = other._mclq_covered;
  _mclq_fishable = other._mclq_fishable;
  _mclq_fatigue = other._mclq_fatigue;
  _mclq_flag_0x10 = other._mclq_flag_0x10;
  _mclq_flag_0x20 = other._mclq_flag_0x20;
  _mclq_flow_0 = other._mclq_flow_0;
  _mclq_flow_1 = other._mclq_flow_1;
  _mclq_depth = other._mclq_depth;
  _render_zeroed_depth = other._render_zeroed_depth;
  _mclq_n_flowvs = other._mclq_n_flowvs;
  _mclq_flowvs = other._mclq_flowvs;
  _vertices = other._vertices;
  _indices_by_lod = other._indices_by_lod;
  _indices_count_by_lod = other._indices_count_by_lod;
  _fatigue_enabled = other._fatigue_enabled;
  _need_data_update = other._need_data_update;
  pos = other.pos;

  // update liquid type and vertex format
  changeLiquidID(_liquid_id);

  return *this;
}

void liquid_layer::copy_data(noggit::chunk_data& data) const
{
  noggit::liquid_layer_data lld;

  lld.liquid_id = _liquid_id;
  lld.liquid_type = _liquid_type;
  lld.subchunk_mask = _subchunks;
  lld.vertices = _vertices;

  data.liquid_layers.push_back(lld);
}

void liquid_layer::create_vertices(float height)
{
  _vertices.clear();

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      _vertices.emplace_back(math::vector_3d(pos.x + UNITSIZE * x, height, pos.z + UNITSIZE * z)
        , default_uv(x, z)
        , 1.f
      );
    }
  }
}

void liquid_layer::save(util::sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const
{
  int min_x = 9, min_z = 9, max_x = 0, max_z = 0;
  bool filled = true;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (hasSubchunk(x, z))
      {
        min_x = std::min(x, min_x);
        min_z = std::min(z, min_z);
        max_x = std::max(x + 1, max_x);
        max_z = std::max(z + 1, max_z);
      }
      else
      {
        filled = false;
      }
    }
  }

  MH2O_Information info;
  std::uint64_t mask = 0;

  info.liquid_id = _liquid_id;
  info.liquid_vertex_format = _liquid_vertex_format;
  info.minHeight = _minimum;
  info.maxHeight = _maximum;
  info.xOffset = min_x;
  info.yOffset = min_z;
  info.width = max_x - min_x;
  info.height = max_z - min_z;

  if (filled)
  {
    info.ofsInfoMask = 0;
  }
  else
  {
    std::uint64_t value = 1;
    for (int z = info.yOffset; z < info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x < info.xOffset + info.width; ++x)
      {
        if (hasSubchunk(x, z))
        {
          mask |= value;
        }
        value <<= 1;
      }
    }

    if (mask > 0)
    {
      info.ofsInfoMask = current_pos - base_pos;
      adt.Insert(current_pos, 8, reinterpret_cast<char*>(&mask));
      current_pos += 8;
    }
  }

  int vertices_count = (info.width + 1) * (info.height + 1);
  info.ofsHeightMap = current_pos - base_pos;

  if (_liquid_vertex_format == 0 || _liquid_vertex_format == 1)
  {
    adt.Extend(vertices_count * sizeof(float));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        memcpy(adt.GetPointer<char>(current_pos).get(), &_vertices[z * 9 + x].position.y, sizeof(float));
        current_pos += sizeof(float);
      }
    }
  }
  // no heightmap/depth data for fatigue chunks
  else if(_fatigue_enabled)
  {
    info.ofsHeightMap = 0;
  }

  if (_liquid_vertex_format == 1)
  {
    adt.Extend(vertices_count * sizeof(mh2o_uv));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        mh2o_uv uv;
        uv.x = static_cast<std::uint16_t>(std::lround(std::min(_vertices[z * 9 + x].uv.x * 255.f, 65535.f)));
        uv.y = static_cast<std::uint16_t>(std::lround(std::min(_vertices[z * 9 + x].uv.y * 255.f, 65535.f)));

        memcpy(adt.GetPointer<char>(current_pos).get(), &uv, sizeof(mh2o_uv));
        current_pos += sizeof(mh2o_uv);
      }
    }
  }

  if (_liquid_vertex_format == 0 || (_liquid_vertex_format == 2 && !_fatigue_enabled))
  {
    adt.Extend(vertices_count * sizeof(std::uint8_t));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        std::uint8_t depth = static_cast<std::uint8_t>(std::lround(std::clamp(_vertices[z * 9 + x].depth * 255.0f, 0.f, 255.f)));
        memcpy(adt.GetPointer<char>(current_pos).get(), &depth, sizeof(std::uint8_t));
        current_pos += sizeof(std::uint8_t);
      }
    }
  }

  memcpy(adt.GetPointer<char>(info_pos).get(), &info, sizeof(MH2O_Information));
  info_pos += sizeof(MH2O_Information);
}

void liquid_layer::changeLiquidID(int id)
{
  _liquid_id = id;

  // vanilla (1.12.1) liquid ids that mclq data uses (see
  // liquid_chunk::from_mclq): the wotlk-shaped accessors in DBC.h read the
  // wrong columns from the vanilla LiquidType.dbc, so the lookup below ends
  // up in the river default for every row; the ids are mapped explicitly
  switch (id)
  {
  case 1: // river
    _liquid_type = 0;
    _liquid_vertex_format = 0;
    _mclq_liquid_type = 4;
    return;
  case 2: // ocean
    _liquid_type = 1;
    // lvf 2 is only used for flat water at height 0
    _liquid_vertex_format = misc::float_equals(_minimum, 0.f) && misc::float_equals(_maximum, 0.f) ? 2 : 0;
    _mclq_liquid_type = 1;
    return;
  case 3: // magma
  case 15: // green lava
    _liquid_type = 2;
    _liquid_vertex_format = 1;
    _mclq_liquid_type = 6;
    return;
  case 4: // slime
    _liquid_type = 3;
    _liquid_vertex_format = 1;
    _mclq_liquid_type = 3;
    return;
  default:
    break;
  }

  try
  {
    DBCFile::Record lLiquidTypeRow = gLiquidTypeDB.getByID(_liquid_id);

    _liquid_type = lLiquidTypeRow.getInt(LiquidTypeDB::Type);

    switch (_liquid_type)
    {
    case 2: // magma
      _mclq_liquid_type = 6;
      _liquid_vertex_format = 1;
      break;
    case 3: // slime
      _mclq_liquid_type = 3;
      _liquid_vertex_format = 1;
      break;
    case 1: // ocean
      // lvf 2 is only used for flat water at height 0
      _liquid_vertex_format = misc::float_equals(_minimum, 0.f) && misc::float_equals(_maximum, 0.f) ? 2 : 0;
      _mclq_liquid_type = 1;
      break;
    default: // river
      _liquid_vertex_format = 0;
      _mclq_liquid_type = 4;
      break;
    }
  }
  catch (...)
  {
  }
}

bool liquid_layer::check_fatigue() const
{
  // only oceans have fatigue
  if (_liquid_type != 1)
  {
    return false;
  }

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (!(hasSubchunk(x, z) && subchunk_at_max_depth(x, z)))
      {
        return false;
      }
    }
  }

  return true;
}

mclq liquid_layer::to_mclq(MH2O_Attributes& attributes) const
{
  // value-initialized: everything not explicitly written below must not end
  // up as stack garbage in the saved file
  mclq mclq_data{};

  mclq_data.min_height = _minimum;
  mclq_data.max_height = _maximum;

  for (int i = 0; i < 8 * 8; ++i)
  {
    if (hasSubchunk(i % 8, i / 8))
    {
      mclq_data.tiles[i].liquid_type = _mclq_liquid_type & 0x7;
      mclq_data.tiles[i].dont_render = 0;

      if (_from_mclq && (_mclq_covered >> i) & 1)
      {
        // tiles that were already covered in the loaded file keep their
        // attribute bits exactly as stored; vanilla marks far more tiles
        // fishable/fatigue than recomputing them would
        mclq_data.tiles[i].fishable = (_mclq_fishable >> i) & 1;
        mclq_data.tiles[i].fatigue = (_mclq_fatigue >> i) & 1;
        mclq_data.tiles[i].flag_0x10 = (_mclq_flag_0x10 >> i) & 1;
        mclq_data.tiles[i].flag_0x20 = (_mclq_flag_0x20 >> i) & 1;
      }
      else
      {
        mclq_data.tiles[i].fishable = (attributes.fishable >> i) & 1;
        mclq_data.tiles[i].fatigue = (attributes.fatigue >> i) & 1;
      }
    }
    else
    {
      mclq_data.tiles[i].liquid_type = 7;
      mclq_data.tiles[i].dont_render = 1;
      mclq_data.tiles[i].fishable = 0;
      mclq_data.tiles[i].fatigue = 0;
    }
  }

  for (int i = 0; i < 9 * 9; ++i)
  {
    // write the height unchanged, including the FLT_MAX sentinel of dry
    // vertices: clamping to [_minimum, _maximum] rewrites it into a real
    // height that lies outside of the stored header range once _minimum and
    // _maximum are recomputed from the rendered vertices only, so the next
    // load would clamp it further and every save would drift
    mclq_data.vertices[i].height = _vertices[i].position.y;

    // magma and slime, chosen by the original mclq type so the union bytes
    // are interpreted the same way they were read
    if (_mclq_liquid_type == 6 || _mclq_liquid_type == 3)
    {
      mclq_data.vertices[i].magma.x = static_cast<std::uint16_t>(std::lround(std::min(_vertices[i].uv.x * 255.f, 65535.f)));
      mclq_data.vertices[i].magma.y = static_cast<std::uint16_t>(std::lround(std::min(_vertices[i].uv.y * 255.f, 65535.f)));
    }
    else
    {
      if (_from_mclq && _render_zeroed_depth[i] && _vertices[i].depth == 0.f)
      {
        // untouched vertex that the loader zeroed for rendering: save the
        // raw byte back instead of the render hack's zero
        mclq_data.vertices[i].water.depth = _mclq_depth[i];
      }
      else
      {
        // round instead of truncate, otherwise the value ratchets down by
        // one on every load/save (e.g. 201/255*255 truncates to 200)
        mclq_data.vertices[i].water.depth = static_cast<std::uint8_t>(std::lround(std::clamp(_vertices[i].depth * 255.f, 0.f, 255.f)));
      }

      if (_from_mclq)
      {
        mclq_data.vertices[i].water.flow_0_pct = _mclq_flow_0[i];
        mclq_data.vertices[i].water.flow_1_pct = _mclq_flow_1[i];
      }
    }
  }

  if (_from_mclq)
  {
    mclq_data.n_flowvs = _mclq_n_flowvs;
    mclq_data.flowvs[0] = _mclq_flowvs[0];
    mclq_data.flowvs[1] = _mclq_flowvs[1];
  }

  return mclq_data;
}

void liquid_layer::update_attributes(MH2O_Attributes& attributes)
{
  if (check_fatigue())
  {
    attributes.fishable = -1;
    attributes.fatigue = -1;

    _fatigue_enabled = true;
  }
  else
  {
    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (hasSubchunk(x, z))
        {
          misc::set_bit(attributes.fishable, x, z, true);

          // only oceans have fatigue
          // warning: not used by TrinityCore
          if (_liquid_type == 1 && subchunk_at_max_depth(x, z))
          {
            misc::set_bit(attributes.fatigue, x, z, true);
          }
        }
      }
    }
  }
}

void liquid_layer::update_indices()
{
  _indices_by_lod.clear();

  int offset = 9 * 9 * _index_in_tile;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      size_t p = z * 9 + x;

      for (int lod_level = 0; lod_level < lod_count; ++lod_level)
      {
        int n = 1 << lod_level;
        if ((z % n) == 0 && (x % n) == 0)
        {
          if (hasSubchunk(x, z, n))
          {
            _indices_by_lod[lod_level].emplace_back(offset + p);
            _indices_by_lod[lod_level].emplace_back(offset + p + n * 9);
            _indices_by_lod[lod_level].emplace_back(offset + p + n * 9 + n);
            _indices_by_lod[lod_level].emplace_back(offset + p + n * 9 + n);
            _indices_by_lod[lod_level].emplace_back(offset + p + n);
            _indices_by_lod[lod_level].emplace_back(offset + p);
          }
        }
        else
        {
          break;
        }
      }
    }
  }

  for (int i = 0; i < lod_count; ++i)
  {
    _indices_count_by_lod[i] = _indices_by_lod[i].size();
  }
}

void liquid_layer::crop(MapChunk* chunk)
{
  if (_maximum < chunk->getMinHeight())
  {
    _subchunks = 0;
  }
  else
  {
    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (hasSubchunk(x, z))
        {
          int water_index = 9 * z + x, terrain_index = 17 * z + x;

          if ( _vertices[water_index +  0].position.y < chunk->vertices[terrain_index +  0].position.y
            && _vertices[water_index +  1].position.y < chunk->vertices[terrain_index +  1].position.y
            && _vertices[water_index +  9].position.y < chunk->vertices[terrain_index + 17].position.y
            && _vertices[water_index + 10].position.y < chunk->vertices[terrain_index + 18].position.y
            )
          {
            setSubchunk(x, z, false);
          }
        }
      }
    }
  }

  update_min_max();
}

void liquid_layer::update_opacity(MapChunk* chunk, float factor)
{
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      update_vertex_opacity(x, z, chunk, factor);
    }
  }
}

// todo: fix uvs so lava lods don't look weird
void liquid_layer::update_underground_vertices_depth(MapChunk* chunk)
{
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      float diff = _vertices[z * 9 + x].position.y - chunk->vertices[z * 17 + x].position.y;

      if (diff < 0.f)
      {
        _vertices[z * 9 + x].depth = 0.f;
        _render_zeroed_depth[z * 9 + x] = 1;
      }
      else
      {
        if (x < 8 && z < 8 && !hasSubchunk(x, z))
        {
          _vertices[z * 9 + x].depth = 0.f;
          _vertices[z * 9 + x + 1].depth = 0.f;
          _vertices[(z + 1) * 9 + x].depth = 0.f;
          _vertices[(z + 1) * 9 + (x + 1)].depth = 0.f;
          _render_zeroed_depth[z * 9 + x] = 1;
          _render_zeroed_depth[z * 9 + x + 1] = 1;
          _render_zeroed_depth[(z + 1) * 9 + x] = 1;
          _render_zeroed_depth[(z + 1) * 9 + (x + 1)] = 1;
        }
      }
    }
  }
}

void liquid_layer::intersect(math::ray const& ray, selection_result* results)
{
  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (hasSubchunk(x, z))
      {
        int id0 = z * 9 + x;
        int id1 = z * 9 + x + 1;
        int id2 = (z + 1) * 9 + x;
        int id3 = (z + 1) * 9 + (x + 1);
        math::vector_3d const& v0 = _vertices[id0].position;
        math::vector_3d const& v1 = _vertices[id1].position;
        math::vector_3d const& v2 = _vertices[id2].position;
        math::vector_3d const& v3 = _vertices[id3].position;

        if (auto dist = ray.intersect_triangle(v0, v1, v2))
        {
          results->emplace_back
          (* dist
            , selected_liquid_layer_type
            ( this
            , std::make_tuple(id0, id1, id2)
            , ray.position(*dist)
            , _liquid_id
            )
          );
        }
        else if (auto dist = ray.intersect_triangle(v2, v3, v1))
        {
          results->emplace_back
          (* dist
            , selected_liquid_layer_type
            ( this
            , std::make_tuple(id2, id3, id1)
            , ray.position(*dist)
            , _liquid_id
            )
          );
        }
      }
    }
  }
}

int liquid_layer::mclq_flag_ordering() const
{
  switch (_mclq_liquid_type)
  {
  case 6: return 2;  // lava
  case 3: return 3;  // slime
  case 1: return 1;  // ocean
  default: return 0; // river

  }
}

bool liquid_layer::subchunk_at_max_depth(int x, int z) const
{
  for (int id_z = z; id_z <= z + 1; ++id_z)
  {
    for (int id_x = x; id_x <= x + 1; ++id_x)
    {
      if (_vertices[id_x + 9 * id_z].depth < 1.f)
      {
        return false;
      }
    }
  }

  return true;
}

bool liquid_layer::hasSubchunk(int x, int z, int size) const
{
  for (int pz = z; pz < z + size; ++pz)
  {
    for (int px = x; px < x + size; ++px)
    {
      if ((_subchunks >> (pz * 8 + px)) & 1)
      {
        return true;
      }
    }
  }
  return false;
}

void liquid_layer::setSubchunk(int x, int z, bool water)
{
  misc::set_bit(_subchunks, x, z, water);
}

void liquid_layer::paintLiquid( math::vector_3d const& cursor_pos
                              , float radius
                              , bool add
                              , math::radians const& angle
                              , math::radians const& orientation
                              , bool lock
                              , math::vector_3d const& origin
                              , bool override_height
                              , MapChunk* chunk
                              , float opacity_factor
                              )
{
  math::vector_3d ref ( lock
                      ? origin
                      : math::vector_3d (cursor_pos.x, cursor_pos.y, cursor_pos.z)
                      );

  int id = 0;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (misc::getShortestDist(cursor_pos, _vertices[id].position, UNITSIZE) <= radius)
      {
        if (add)
        {
          for (int index : {id, id + 1, id + 9, id + 10})
          {
            bool no_subchunk = !hasSubchunk(x, z);
            bool in_range = misc::dist(cursor_pos, _vertices[index].position) <= radius;

            if (no_subchunk || (in_range && override_height))
            {
              _vertices[index].position.y = misc::angledHeight(ref, _vertices[index].position, angle, orientation);
            }
            if (no_subchunk || in_range)
            {
              update_vertex_opacity(index % 9, index / 9, chunk, opacity_factor);
            }
          }
        }
        setSubchunk(x, z, add);
      }

      id++;
    }
    // to go to the next row of subchunks
    id++;
  }

  update_min_max();
}

void liquid_layer::update_min_max()
{
  _minimum = std::numeric_limits<float>::max();
  _maximum = std::numeric_limits<float>::lowest();
  int x = 0, z = 0;

  for (liquid_vertex& v : _vertices)
  {
    if (hasSubchunk(std::min(x, 7), std::min(z, 7)))
    {
      _maximum = std::max(_maximum, v.position.y);
      _minimum = std::min(_minimum, v.position.y);
    }

    if (++x == 9)
    {
      z++;
      x = 0;
    }
  }

  _center = math::vector_3d(pos.x + CHUNKSIZE * 0.5f, (_maximum + _minimum) * 0.5f, pos.z + CHUNKSIZE * 0.5f);

  // lvf = 2 means the liquid height is 0, switch to lvf 0 when that's not the case
  if (_liquid_vertex_format == 2 && (!misc::float_equals(0.f, _minimum) || !misc::float_equals(0.f, _maximum)))
  {
    _liquid_vertex_format = 0;
  }
  // use lvf 2 when possible to save space
  else if (_liquid_vertex_format == 0 && misc::float_equals(0.f, _minimum) && misc::float_equals(0.f, _maximum))
  {
    _liquid_vertex_format = 2;
  }

  _fatigue_enabled = check_fatigue();
}

void liquid_layer::copy_subchunk_height(int x, int z, liquid_layer const& from)
{
  int id = 9 * z + x;

  for (int index : {id, id + 1, id + 9, id + 10})
  {
    _vertices[index].position.y = from._vertices[index].position.y;
  }

  setSubchunk(x, z, true);
}

void liquid_layer::update_vertex_opacity(int x, int z, MapChunk* chunk, float factor)
{
  float diff = _vertices[z * 9 + x].position.y - chunk->vertices[z * 17 + x].position.y;
  _vertices[z * 9 + x].depth = std::clamp((diff + 1.0f) * factor, 0.f, 1.f);

  // an explicit edit overrides the depth the loader zeroed for rendering, so
  // the raw stored byte is no longer authoritative for this vertex: without
  // this, editing the depth back to 0 would silently save the old byte
  _render_zeroed_depth[z * 9 + x] = 0;
}

int liquid_layer::get_lod_level(math::vector_3d const& camera_pos) const
{
  auto const dist ((_center - camera_pos).length());

  float f = std::min(1.f, dist / (500.f * lod_count));
  return std::clamp(static_cast<int>(f * lod_count), 0, lod_count - 1);
}

void liquid_layer::set_lod_level(int lod_level, std::vector<void*>& indices_offsets, std::vector<int>& indices_count)
{
  if (lod_level != _current_lod_level)
  {
    _current_lod_level = lod_level;
    _current_lod_indices_count = _indices_count_by_lod[_current_lod_level];

    indices_count[_index_in_tile] = _indices_count_by_lod[_current_lod_level];
    indices_offsets[_index_in_tile] = static_cast<char*>(0) + _index_in_tile * indice_buffer_size_required + lod_level_offset[_current_lod_level];
  }
}

void liquid_layer::update_data(liquid_render& render)
{
  update_indices();

  liquid_layer_ubo_data data = render.ubo_data(_liquid_id);
  int offset = 0;

  for (int i = 0; i < lod_count; ++i)
  {
    gl.bufferSubData(GL_ELEMENT_ARRAY_BUFFER, _index_in_tile * indice_buffer_size_required + offset, _indices_by_lod.at(i).size() * sizeof(liquid_indice), _indices_by_lod.at(i).data());

    offset += max_indices_per_lod_level[i] * sizeof(liquid_indice);
  }

  gl.bufferSubData(GL_ARRAY_BUFFER, _index_in_tile * vertex_buffer_size_required, _vertices.size() * sizeof(liquid_vertex), _vertices.data());
  gl.bufferSubData(GL_UNIFORM_BUFFER, _index_in_tile * sizeof(liquid_layer_ubo_data), sizeof(liquid_layer_ubo_data), &data);

  _indices_by_lod.clear();

  _need_data_update = false;
}

void liquid_layer::update_indices_info(std::vector<void*>& indices_offsets, std::vector<int>& indices_count)
{
  indices_count.push_back(_indices_count_by_lod[_current_lod_level]);
  indices_offsets.push_back(static_cast<char*>(0) + _index_in_tile * indice_buffer_size_required + lod_level_offset[_current_lod_level]);
}
