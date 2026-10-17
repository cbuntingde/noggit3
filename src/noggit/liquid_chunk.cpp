// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/liquid_chunk.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/liquid_tile.hpp>
#include <noggit/MPQ.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>

#include <algorithm>

liquid_chunk::liquid_chunk(float x, float z, bool use_mclq_green_lava, liquid_tile* parent_tile)
  : xbase(x)
  , zbase(z)
  , vmin(x, 0.f, z)
  , vmax(x + CHUNKSIZE, 0.f, z + CHUNKSIZE)
  , _use_mclq_green_lava(use_mclq_green_lava)
  , _liquid_tile(parent_tile)
{
}

void liquid_chunk::from_mclq(std::vector<mclq>& layers)
{
  math::vector_3d pos(xbase, 0.0f, zbase);

  for (mclq& liquid : layers)
  {
    std::uint8_t mclq_liquid_type = 0;

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        mclq_tile const& tile = liquid.tiles[z * 8 + x];

        misc::bit_or(attributes.fishable, x, z, tile.fishable);
        misc::bit_or(attributes.fatigue, x, z, tile.fatigue);

        if (!tile.dont_render)
        {
          mclq_liquid_type = tile.liquid_type;
        }
      }
    }

    switch (mclq_liquid_type)
    {
      case 1:_layers.emplace_back(pos, liquid, 2); break;
      case 3:_layers.emplace_back(pos, liquid, 4); break;
      case 4:_layers.emplace_back(pos, liquid, 1); break;
      case 6:_layers.emplace_back(pos, liquid, (_use_mclq_green_lava ? 15 : 3)); break;
      default:
        LogError << "Invalid/unhandled MCLQ liquid type" << std::endl;
        break;
    }
  }

  update_layers();

  _liquid_tile->require_buffer_regen();
  _liquid_tile->set_has_water();
}

void liquid_chunk::fromFile(MPQFile &f, size_t basePos)
{
  MH2O_Header header;
  f.read(&header, sizeof(MH2O_Header));

  if (!header.nLayers)
  {
    return;
  }

  //render
  if (header.ofsRenderMask)
  {
    f.seek(basePos + header.ofsRenderMask);
    f.read(&attributes, sizeof(MH2O_Attributes));
  }

  for (std::size_t k = 0; k < header.nLayers; ++k)
  {
    MH2O_Information info;
    uint64_t infoMask = 0xFFFFFFFFFFFFFFFF; // default = all water

    //info
    f.seek(basePos + header.ofsInformation + sizeof(MH2O_Information)* k);
    f.read(&info, sizeof(MH2O_Information));

    //mask
    if (info.ofsInfoMask > 0 && info.height > 0)
    {
      size_t bitmask_size = static_cast<size_t>(std::ceil(info.height * info.width / 8.0f));

      f.seek(info.ofsInfoMask + basePos);
      // only read the relevant data
      f.read(&infoMask, bitmask_size);
    }

    math::vector_3d pos(xbase, 0.0f, zbase);
    _layers.emplace_back(f, basePos, pos, info, infoMask);
  }

  update_layers();
}


void liquid_chunk::update_attributes()
{
  attributes.fishable = 0;
  attributes.fatigue = 0;

  for (liquid_layer& layer : _layers)
  {
    layer.update_attributes(attributes);
  }
}

void liquid_chunk::save(util::sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos)
{
  MH2O_Header header;

  // remove empty layers
  cleanup();
  update_attributes();

  if (hasData(0))
  {
    header.nLayers = _layer_count;

    // fagique only for single layer ocean chunk
    bool fatigue = _layers[0].has_fatigue();

    if (!fatigue)
    {
      header.ofsRenderMask = current_pos - base_pos;
      adt.Insert(current_pos, sizeof(MH2O_Attributes), reinterpret_cast<char*>(&attributes));
      current_pos += sizeof(MH2O_Attributes);
    }
    else
    {
      header.ofsRenderMask = 0;
    }

    header.ofsInformation = current_pos - base_pos;
    int info_pos = current_pos;

    std::size_t info_size = sizeof(MH2O_Information) * _layer_count;
    current_pos += info_size;

    adt.Extend(info_size);

    for (liquid_layer& layer : _layers)
    {
      layer.save(adt, base_pos, info_pos, current_pos);
    }
  }

  memcpy(adt.GetPointer<char>(header_pos).get(), &header, sizeof(MH2O_Header));
  header_pos += sizeof(MH2O_Header);
}

int liquid_chunk::save_mclq(util::sExtendableArray& adt, int mcnk_pos, int& current_pos)
{
  // remove empty layers
  cleanup();
  update_attributes();

  if (!hasData(0))
  {
    return 0;
  }

  // size the chunk from the layers actually written (cleanup just dropped the
  // empty ones) so the mcnk header size always matches the serialized bytes
  int const stored_layers = static_cast<int>(_layers.size());
  int const chunk_size = 8 + stored_layers * static_cast<int>(sizeof(mclq));

  adt.Extend(chunk_size);
  // size seems to be 0 in vanilla adts in the mclq chunk's header and set right in the mcnk header (layer_size * n_layer + 8)
  SetChunkHeader(adt, current_pos, 'MCLQ', 0);

  current_pos += 8;

  // it's possible to merge layers when they don't overlap (liquids using the same vertice, but at different height)
  // layer ordering seems to matter, having a lava layer then a river layer causes the lava layer to not render ingame
  // sorting order seems to be dependant on the flag ordering in the mcnk's header
  std::vector<std::pair<mclq, int>> mclq_layers;

  for (liquid_layer const& layer : _layers)
  {
    switch (layer.mclq_liquid_type())
    {
    case 6: // lava
      adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_magma = 1;
      break;
    case 3: // slime
      adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_slime = 1;
      break;
    case 1: // ocean
      adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_ocean = 1;
      break;
    default: // river
      adt.GetPointer<MapChunkHeader>(mcnk_pos + 8)->flags.flags.lq_river = 1;
      break;
    }

    mclq_layers.push_back({ layer.to_mclq(attributes), layer.mclq_flag_ordering() });
  }

  auto cmp = [](std::pair<mclq, int> const& a, std::pair<mclq, int> const& b)
  {
    return a.second < b.second;
  };

  // sort the layers by flag order
  std::sort(mclq_layers.begin(), mclq_layers.end(), cmp);

  for (auto const& mclq_layer : mclq_layers)
  {
    std::memcpy(adt.GetPointer<char>(current_pos).get(), &mclq_layer.first, sizeof(mclq));
    current_pos += sizeof(mclq);
  }

  return chunk_size;
}

void liquid_chunk::copy_data(noggit::chunk_data& data) const
{
  data.liquid_layer_count = _layers.size();
  data.liquid_attributes = attributes;

  for (liquid_layer const& layer : _layers)
  {
    layer.copy_data(data);
  }
}

void liquid_chunk::override_data(noggit::chunk_data const& data, noggit::chunk_override_params const&)
{
  _layers.clear();

  attributes = data.liquid_attributes;
  _layer_count = data.liquid_layer_count;

  for (noggit::liquid_layer_data const& layer_data : data.liquid_layers)
  {
    _layers.emplace_back(math::vector_3d(data.origin.x, 0.f, data.origin.z), layer_data);
  }

  update_layers();
  _liquid_tile->require_buffer_regen();
  _liquid_tile->set_has_water();
}
void liquid_chunk::set_preview_data(noggit::chunk_data const& data, noggit::chunk_override_params const&)
{
  _preview_layers.clear();

  _layer_count = data.liquid_layer_count;

  for (noggit::liquid_layer_data const& layer_data : data.liquid_layers)
  {
    _preview_layers.emplace_back(math::vector_3d(data.origin.x, 0.f, data.origin.z), layer_data);
  }

  update_layers();
  _liquid_tile->require_buffer_regen();
  _liquid_tile->set_has_water();
}

void liquid_chunk::clear_preview()
{
  if (!_preview_layers.empty())
  {
    _preview_layers.clear();

    update_layers();
    _liquid_tile->require_buffer_regen();
    _liquid_tile->set_has_water();
  }
}

void liquid_chunk::autoGen(MapChunk *chunk, float factor)
{
  for (liquid_layer& layer : _layers)
  {
    layer.update_opacity(chunk, factor);
  }
  update_layers();
}

void liquid_chunk::auto_update_water_opacity(MapChunk* chunk)
{
  for (liquid_layer& layer : _layers)
  {
    if (layer.liquid_type() == 0)
    {
      layer.update_opacity(chunk, noggit::river_opacity);
    }
    else if (layer.liquid_type() == 1)
    {
      layer.update_opacity(chunk, noggit::ocean_opacity);
    }
  }

  update_layers();
}

void liquid_chunk::update_underground_vertices_depth(MapChunk *chunk)
{
  for (liquid_layer& layer : _layers)
  {
    layer.update_underground_vertices_depth(chunk);
  }
}


void liquid_chunk::CropWater(MapChunk* chunkTerrain)
{
  for (liquid_layer& layer : _layers)
  {
    layer.crop(chunkTerrain);
  }
  update_layers();
}

int liquid_chunk::getType(size_t layer) const
{
  return hasData(layer) ? _layers[layer].liquidID() : 0;
}

void liquid_chunk::setType(int type, size_t layer)
{
  if(hasData(layer))
  {
    _layers[layer].changeLiquidID(type);
  }
}

bool liquid_chunk::is_visible ( const float& cull_distance
                            , const math::frustum& frustum
                            , const math::vector_3d& camera
                            , display_mode display
                            ) const
{
  static const float chunk_radius = std::sqrt (CHUNKSIZE * CHUNKSIZE / 2.0f);

  if (_layer_count < 1)
  {
    return false;
  }

  float dist = display == display_mode::in_3D
             ? (camera - vcenter).length() - chunk_radius
             : std::abs(camera.y - vmax.y);

  return frustum.intersects (_intersect_points)
      && dist < cull_distance;
}

void liquid_chunk::intersect(math::ray const& ray, selection_result* results)
{
  if (!hasData(0) || !ray.intersect_bounds(vmin, vmax))
  {
    return;
  }

  for (liquid_layer& layer : _layers)
  {
    layer.intersect(ray, results);
  }
}

void liquid_chunk::update_layers()
{
  // only used for non preview operations
  _layer_count = _layers.size();

  auto& layers(displayed_layers());

  if (layers.size() > 0)
  {
    vmin.y = std::numeric_limits<float>().max();
    vmax.y = std::numeric_limits<float>().min();

    for (liquid_layer& layer : layers)
    {
      layer.update_indices();
      vmin.y = std::min(vmin.y, layer.min());
      vmax.y = std::max(vmax.y, layer.max());
    }
  }
  else // default to 0 for empty chunks
  {
    vmin.y = vmax.y = 0.f;
  }

  vcenter = (vmin + vmax) * 0.5f;

  _intersect_points.clear();
  _intersect_points = misc::intersection_points(vmin, vmax);

  _liquid_tile->require_buffer_update();
}

bool liquid_chunk::hasData(size_t layer) const
{
  return _layer_count > layer;
}


void liquid_chunk::paintLiquid( math::vector_3d const& pos
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
                            )
{
  if (override_liquid_id && !override_height)
  {
    bool layer_found = false;
    for (liquid_layer& layer : _layers)
    {
      if (layer.liquidID() == liquid_id)
      {
        copy_height_to_layer(layer, pos, radius);
        layer_found = true;
        break;
      }
    }

    if (!layer_found)
    {
      liquid_layer layer(math::vector_3d(xbase, 0.0f, zbase), pos.y, liquid_id);

      copy_height_to_layer(layer, pos, radius);

      _layers.push_back(layer);
      _liquid_tile->require_buffer_regen();
    }
  }

  bool painted = false;
  for (liquid_layer& layer : _layers)
  {
    // remove the water on all layers or paint the layer with selected id
    if (!add || layer.liquidID() == liquid_id || !override_liquid_id)
    {
      layer.paintLiquid(pos, radius, add, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
      painted = true;
    }
    else
    {
      layer.paintLiquid(pos, radius, false, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    }
  }

  cleanup();

  if (!add || painted)
  {
    update_layers();
    return;
  }

  if (hasData(0))
  {
    liquid_layer layer(_layers[0]);
    layer.clear(); // remove the liquid to not override the other layer
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    layer.changeLiquidID(liquid_id);
    _layers.push_back(layer);
  }
  else
  {
    liquid_layer layer(math::vector_3d(xbase, 0.0f, zbase), pos.y, liquid_id);
    layer.paintLiquid(pos, radius, true, angle, orientation, lock, origin, override_height, chunk, opacity_factor);
    _layers.push_back(layer);

    _liquid_tile->set_has_water();
  }

  update_layers();
  _liquid_tile->require_buffer_regen();
}

void liquid_chunk::clear_layers()
{
  _layers.clear();
  _liquid_tile->require_buffer_regen();

  update_layers();
  update_attributes();
}

void liquid_chunk::cleanup()
{
  for (int i = _layer_count - 1; i >= 0; --i)
  {
    if (_layers[i].empty())
    {
      _layers.erase(_layers.begin() + i);
      _liquid_tile->require_buffer_regen();
    }
  }

  update_layers();
}

void liquid_chunk::copy_height_to_layer(liquid_layer& target, math::vector_3d const& pos, float radius)
{
  for (liquid_layer& layer : _layers)
  {
    if (layer.liquidID() == target.liquidID())
    {
      continue;
    }

    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (misc::getShortestDist(pos.x, pos.z, xbase + x*UNITSIZE, zbase + z*UNITSIZE, UNITSIZE) <= radius)
        {
          if (layer.hasSubchunk(x, z))
          {
            target.copy_subchunk_height(x, z, layer);
          }
        }
      }
    }
  }
}

void liquid_chunk::upload_data(int& index_in_tile, liquid_render& render)
{
  for (liquid_layer& layer : displayed_layers())
  {
    layer.upload_data(index_in_tile++, render);
  }
}

void liquid_chunk::update_data(liquid_render& render)
{
  for (liquid_layer& layer : displayed_layers())
  {
    layer.update_data(render);
  }
}

void liquid_chunk::update_indices_info(std::vector<void*>& indices_offsets, std::vector<int>& indices_count)
{
  for (liquid_layer& layer : displayed_layers())
  {
    layer.update_indices_info(indices_offsets, indices_count);
  }
}
void liquid_chunk::update_lod_level(math::vector_3d const& camera_pos, std::vector<void*>& indices_offsets, std::vector<int>& indices_count)
{
  auto& layers(displayed_layers());
  if (!layers.empty())
  {
    int lod = layers[0].get_lod_level(camera_pos);

    for (liquid_layer& layer : layers)
    {
      layer.set_lod_level(lod, indices_offsets, indices_count);
    }
  }
}
