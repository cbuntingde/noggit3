// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Brush.h>
#include <noggit/chunk_mover.hpp>
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/World.h>
#include <noggit/texture_set.hpp>
#include <noggit/MapChunk.h>

#include <algorithm>    // std::min
#include <cstdint>
#include <iostream>     // std::cout
#include <stdexcept>


TextureSet::TextureSet ( MapChunkHeader const& header
                       , MPQFile* f
                       , size_t base
                       , MapTile* tile
                       , bool use_big_alphamaps
                       , bool do_not_fix_alpha_map
                       , bool do_not_convert_alphamaps
                       , chunk_shadow* shadow
                       )
  : nTextures(header.nLayers)
  , _do_not_convert_alphamaps(do_not_convert_alphamaps)
{
  for (int i = 0; i < 64; ++i)
  {
    const size_t array_index(i / 4);
    // it's a uint2 array so we need to read the bits as they come on the disk,
    // this means reading the highest bits from the uint8 first
    const size_t bit_index((3-((i) % 4)) * 2);

    _lod_texture_map.push_back(((header.low_quality_texture_map[array_index]) >> (bit_index)) & 3);
  }

  if (nTextures > 4)
  {
    throw std::runtime_error ("TextureSet: too many layers.");
  }

  if (nTextures)
  {
    f->seek(base + header.ofsLayer + 8);

    for (size_t i = 0; i<nTextures; ++i)
    {
      if (f->read (&_layers_info[i], sizeof(ENTRY_MCLY)) != sizeof(ENTRY_MCLY))
      {
        throw std::runtime_error ("TextureSet: truncated MCLY.");
      }

      if (_layers_info[i].textureID >= tile->mTextureFilenames.size())
      {
        throw std::out_of_range ("TextureSet: textureID out of range.");
      }
      _textures.push_back(tile->mTextureFilenames[_layers_info[i].textureID]);
    }

    size_t alpha_base = base + header.ofsAlpha + 8;

    for (unsigned int layer = 0; layer < nTextures; ++layer)
    {
      if (_layers_info[layer].flags & 0x100)
      {
        if (_layers_info[layer].ofsAlpha > header.sizeAlpha)
        {
          throw std::out_of_range ("TextureSet: ofsAlpha out of range.");
        }
        std::uint64_t const next_ofs = (layer == nTextures - 1)
          ? static_cast<std::uint64_t>(header.sizeAlpha > 8 ? header.sizeAlpha - 8 : 0)
          : _layers_info[layer + 1].ofsAlpha;
        if (next_ofs < _layers_info[layer].ofsAlpha)
        {
          throw std::runtime_error ("TextureSet: invalid alpha offsets.");
        }
        std::uint64_t const layer_size_64 = next_ofs - _layers_info[layer].ofsAlpha;
        if (layer_size_64 > 65536ULL)
        {
          throw std::runtime_error ("TextureSet: implausible alpha size.");
        }
        int layer_size = static_cast<int>(layer_size_64);

        // make sure to load using the right alphamap format
        // todo: mark adt as changed if the format doesn't match
        use_big_alphamaps = (layer_size == 4096 || _layers_info[layer].flags & 0x200);

        f->seek (alpha_base + _layers_info[layer].ofsAlpha);
        alphamaps[layer - 1] = std::make_unique<Alphamap> (f, _layers_info[layer].flags, use_big_alphamaps, do_not_fix_alpha_map);
      }
      else if(layer > 0) // create empty alphamap
      {
        alphamaps[layer - 1] = std::make_unique<Alphamap>();
      }
    }

    // always use big alpha for editing / rendering
    if (!use_big_alphamaps && !_do_not_convert_alphamaps)
    {
      convertToBigAlpha();
    }


    if (nTextures)
    {
      _setup_amap_data = std::make_unique<std::array<std::uint8_t, 4 * 64 * 64>>();
      std::uint8_t* amap = _setup_amap_data.get()->data();
      std::uint8_t const* alpha_ptr[3];

      bool use_shadow = shadow != nullptr;

      for (int i = 0; i < nTextures - 1; ++i)
      {
        alpha_ptr[i] = alphamaps[i]->getAlpha();
      }

      for (int i = 0; i < 64 * 64; ++i)
      {
        for (int alpha_id = 0; alpha_id < 3; ++alpha_id)
        {
          amap[i * 4 + alpha_id] = ((alpha_id < nTextures - 1)
            ? *(alpha_ptr[alpha_id]++)
            : 0
            );
        }

        amap[i * 4 + 3] = use_shadow ? ((shadow->data[i / 64] >> (i % 64)) & 1) * 255 : 0;
      }
      _first_amap_setup = true;
    }
    _need_amap_update = true;
  }
}


void TextureSet::copy_data(noggit::chunk_data& data)
{
  apply_alpha_changes();

  data.texture_count = nTextures;

  for (int i = 0; i < nTextures; ++i)
  {
    data.textures[i] = _textures[i];
    data.texture_flags[i] = _layers_info[i];

    if (i > 0)
    {
      data.alphamaps[i - 1].setAlpha(alphamaps[i - 1].get()->getAlpha());
    }
  }
}

void TextureSet::override_data(noggit::chunk_data& data, noggit::chunk_override_params const& params)
{
  if (params.alphamaps)
  {
    apply_alpha_changes();
  }

  int old_tex_count = nTextures;

  if (params.textures)
  {
    nTextures = data.texture_count;
    _textures.resize(nTextures);
  }

  for (int i = 0; i < nTextures; ++i)
  {
    if (params.textures)
    {
      _textures[i] = data.textures[i];
      _layers_info[i] = data.texture_flags[i];
    }

    if (i > 0)
    {
      if (params.alphamaps)
      {
        alphamaps[i - 1] = std::make_unique<Alphamap>();
        alphamaps[i - 1]->setAlpha(data.alphamaps[i - 1].getAlpha());
      }
      // create empty alphamap otherwise to match the texture count
      else if(i >= old_tex_count)
      {
        alphamaps[i - 1] = std::make_unique<Alphamap>();
      }
    }
  }
  for (int i = nTextures; i < 4; ++i)
  {
    if (i > 0)
    {
      alphamaps[i - 1].reset();

    }

    _layers_info[i] = ENTRY_MCLY();
  }

  require_update();
}

int TextureSet::addTexture (scoped_blp_texture_reference texture)
{
  int texLevel = -1;

  if (nTextures < 4U)
  {
    texLevel = nTextures;
    nTextures++;

    _textures.push_back(texture->filename);
    _layers_info[texLevel] = ENTRY_MCLY();

    if (texLevel)
    {
      alphamaps[texLevel - 1] = std::make_unique<Alphamap>();
    }

    if (tmp_edit_values && nTextures == 1)
    {
      tmp_edit_values.get()->map[0].fill(255.f);
    }
  }

  require_update();

  return texLevel;
}

void TextureSet::replace_texture (scoped_blp_texture_reference const& texture_to_replace, scoped_blp_texture_reference replacement_texture)
{
  int texture_to_replace_level = -1, replacement_texture_level = -1;

  for (size_t i = 0; i < nTextures; ++i)
  {
    if (_textures[i] == texture_to_replace->filename)
    {
      texture_to_replace_level = i;
    }
    else if (_textures[i] == replacement_texture->filename)
    {
      replacement_texture_level = i;
    }
  }

  if (texture_to_replace_level != -1)
  {
    _textures[texture_to_replace_level] = replacement_texture->filename;

    // prevent texture duplication
    if (replacement_texture_level != -1 && replacement_texture_level != texture_to_replace_level)
    {
      // temp alphamap changes are applied in here
      merge_layers(texture_to_replace_level, replacement_texture_level);
    }
  }
}

void TextureSet::swap_layers(int layer_1, int layer_2)
{
  int lower_texture_id = std::min(layer_1, layer_2);
  int upper_texture_id = std::max(layer_1, layer_2);

  if (lower_texture_id == upper_texture_id)
  {
    return;
  }

  if (lower_texture_id > upper_texture_id)
  {
    std::swap(lower_texture_id, upper_texture_id);
  }

  if (lower_texture_id >= 0 && upper_texture_id >= 0 && lower_texture_id < nTextures && upper_texture_id < nTextures)
  {
    apply_alpha_changes();

    std::swap(_textures[lower_texture_id], _textures[upper_texture_id]);
    std::swap(_layers_info[lower_texture_id], _layers_info[upper_texture_id]);

    int a1 = lower_texture_id - 1, a2 = upper_texture_id - 1;

    if (lower_texture_id)
    {
      std::swap(alphamaps[a1], alphamaps[a2]);
    }
    else
    {
      uint8_t alpha[4096];

      for (int i = 0; i < 4096; ++i)
      {
        alpha[i] = 255 - sum_alpha(i);
      }

      alphamaps[a2]->setAlpha(alpha);
    }

    require_update();
  }
}

void TextureSet::eraseTextures()
{
  if (nTextures == 0)
  {
    return;
  }

  for (int i = 0; i < 4; ++i)
  {
    if (i > 0)
    {
      alphamaps[i - 1].reset();
    }
    _layers_info[i] = ENTRY_MCLY();
  }

  nTextures = 0;

  _textures.clear();
  _lod_texture_map.resize(8 * 8);
  memset(_lod_texture_map.data(), 0, 64 * sizeof(std::uint8_t));

  tmp_edit_values.reset();

  require_update();
}

void TextureSet::eraseTexture(size_t id)
{
  if (id >= nTextures)
  {
    return;
  }

  // shift textures above
  for (size_t i = id; i < nTextures - 1; i++)
  {
    if (i)
    {
      alphamaps[i - 1].reset();
      std::swap (alphamaps[i - 1], alphamaps[i]);
    }

    if (tmp_edit_values)
    {
      tmp_edit_values.get()->map[i].swap(tmp_edit_values.get()->map[i+1]);
    }

    _layers_info[i] = _layers_info[i + 1];
    _textures[i] = _textures[i + 1];
  }

  if (nTextures > 1)
  {
    alphamaps[nTextures - 2].reset();
  }

  nTextures--;
  _textures.erase(_textures.begin() + nTextures);

  // erase the old info as a precaution but it's overriden when adding a new texture
  _layers_info[nTextures] = ENTRY_MCLY();

  // set the default values for the temporary alphamap too
  if (tmp_edit_values)
  {
    tmp_edit_values.get()->map[nTextures].fill(0.f);
  }

  require_update();
}

bool TextureSet::canPaintTexture(std::string const& texture)
{
  if (nTextures)
  {
    for (size_t k = 0; k < nTextures; ++k)
    {
      if (_textures[k] == texture)
      {
        return true;
      }
    }

    return nTextures < 4;
  }

  return true;
}

math::vector_3d TextureSet::anim_param(int layer) const
{
  if (layer >= nTextures)
  {
    return { 1.f, 0.f, 0.f };
  }

  return misc::texture_anim_params(_layers_info[layer].flags);
}

bool TextureSet::eraseUnusedTextures(float threshold)
{
  if (threshold >= 255.f)
  {
    eraseTextures();
  }

  if (nTextures < 2)
  {
    return false;
  }

  int i_threshold = static_cast<int>(threshold);

  std::set<int> visible_tex;

  if (tmp_edit_values)
  {
    auto& amaps = *tmp_edit_values.get();

    for (int layer = 0; layer < nTextures && visible_tex.size() < nTextures; ++layer)
    {
      for (int i = 0; i < 4096; ++i)
      {
        // use 0.01 to account for floating point imprecision
        // while not preventing very low pressure brush from painting correctly
        if (amaps[layer][i] >= threshold)
        {
          visible_tex.emplace(layer);
          break; // texture visible, go to the next layer
        }
      }
    }
  }
  else
  {
    for (int i = 0; i < 4096 && visible_tex.size() < nTextures; ++i)
    {
      uint8_t sum = 0;
      for (int n = 0; n < nTextures - 1; ++n)
      {
        uint8_t a = alphamaps[n]->getAlpha(i);
        sum += a;
        if (a > i_threshold)
        {
          visible_tex.emplace(n + 1);
        }
      }

      // base layer visible
      if (sum < 255)
      {
        visible_tex.emplace(0);
      }
    }
  }

  if (visible_tex.size() < nTextures)
  {
    for (int i = nTextures - 1; i >= 0; --i)
    {
      if (visible_tex.find(i) == visible_tex.end())
      {
        eraseTexture(i);
      }
    }

    require_update();

    return true;
  }

  return false;
}

int TextureSet::get_texture_index_or_add (scoped_blp_texture_reference texture, float target)
{
  for (int i = 0; i < nTextures; ++i)
  {
    if (_textures[i] == texture->filename)
    {
      return i;
    }
  }

  // don't add a texture for nothing
  if (target == 0)
  {
    return -1;
  }

  if (nTextures == 4 && !eraseUnusedTextures())
  {
    return -1;
  }

  return addTexture (std::move (texture));
}

bool TextureSet::paintTexture(float xbase, float zbase, float x, float z, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture)
{
  bool changed = false;

  float zPos, xPos, dist, radius;

  // todo: investigate the root cause
  // shift brush origin to avoid disconnects at the chunks' borders
  if (x < xbase)
  {
    float chunk_dist = std::abs(x - (std::fmod(x, CHUNKSIZE)) - xbase) / CHUNKSIZE;
    x += TEXDETAILSIZE * chunk_dist;
  }
  if (x > xbase + CHUNKSIZE)
  {
    float chunk_dist = std::abs(x - (std::fmod(x, CHUNKSIZE)) - xbase) / CHUNKSIZE;
    x -= TEXDETAILSIZE * chunk_dist;
  }
  if (z < zbase)
  {
    float chunk_dist = std::abs(z - (std::fmod(z, CHUNKSIZE)) - zbase) / CHUNKSIZE;
    z += TEXDETAILSIZE * chunk_dist;
  }
  if (z > zbase + CHUNKSIZE)
  {
    float chunk_dist = std::abs(z - (std::fmod(z, CHUNKSIZE)) - zbase) / CHUNKSIZE;
    z -= TEXDETAILSIZE * chunk_dist;
  }

  int tex_layer = get_texture_index_or_add (std::move (texture), strength);

  if (tex_layer == -1 || nTextures == 1)
  {
    return nTextures == 1;
  }

  radius = brush->get_radius();

  if (misc::getShortestDist(x, z, xbase, zbase, CHUNKSIZE) > radius)
  {
    return changed;
  }

  create_temporary_alphamaps_if_needed();
  auto& amaps = *tmp_edit_values.get();

  zPos = zbase;

  for (int j = 0; j < 64; j++)
  {
    xPos = xbase;
    for (int i = 0; i < 64; ++i)
    {
      dist = misc::getShortestDist(x, z, xPos, zPos, TEXDETAILSIZE);

      if (dist <= radius)
      {
        // use double for more precision
        std::array<double,4> alpha_values;
        double total = 0.;

        for (int n = 0; n < 4; ++n)
        {
          total += alpha_values[n] = amaps[n][i + 64 * j];
        }

        double current_alpha = alpha_values[tex_layer];
        double sum_other_alphas = (total - current_alpha);
        double alpha_change = (strength - current_alpha) * pressure * brush->value_at_dist(dist);

        // alpha too low, set it to 0 directly
        if (alpha_change < 0. && current_alpha + alpha_change < 1.)
        {
          alpha_change = -current_alpha;
        }

        if (!misc::float_equals(current_alpha, strength))
        {
          if (sum_other_alphas < 1.)
          {
            // alpha is currently at 254/255 -> set it at 255 and clear the rest of the values
            if (alpha_change > 0.f)
            {
              for (int layer = 0; layer < nTextures; ++layer)
              {
                alpha_values[layer] = layer == tex_layer ? 255. : 0.f;
              }
            }
            // all the other textures amount for less an 1/255 -> add the alpha_change (negative) to current texture and remove it from the first non current texture, clear the rest
            else
            {
              bool change_applied = false;

              for (int layer = 0; layer < nTextures; ++layer)
              {
                if (layer == tex_layer)
                {
                  alpha_values[layer] += alpha_change;
                }
                else
                {
                  if (!change_applied)
                  {
                    alpha_values[layer] -= alpha_change;
                  }
                  else
                  {
                    alpha_values[tex_layer] += alpha_values[layer];
                    alpha_values[layer] = 0.;
                  }

                  change_applied = true;
                }
              }
            }
          }
          else
          {
            for (int layer = 0; layer < nTextures; ++layer)
            {
              if (layer == tex_layer)
              {
                alpha_values[layer] += alpha_change;
              }
              else
              {
                alpha_values[layer] -= alpha_change * alpha_values[layer] / sum_other_alphas;

                // clear values too low to be visible
                if (alpha_values[layer] < 1.)
                {
                  alpha_values[tex_layer] += alpha_values[layer];
                  alpha_values[layer] = 0.f;
                }
              }
            }
          }

          double total_final = std::accumulate(alpha_values.begin(), alpha_values.end(), 0.);

          // failsafe in case the sum of all alpha values deviate
          if (std::abs(total_final - 255.) > 0.001)
          {
            for (double& d : alpha_values)
            {
              d = d * 255. / total_final;
            }
          }

          for (int n = 0; n < 4; ++n)
          {
            amaps[n][i + 64 * j] = static_cast<float>(alpha_values[n]);
          }

          changed = true;
        }
      }

      xPos += TEXDETAILSIZE;
    }
    zPos += TEXDETAILSIZE;
  }

  if (!changed)
  {
    return false;
  }

  // cleanup
  eraseUnusedTextures();

  require_update();

  return true;
}

bool TextureSet::replace_texture( float xbase
                                , float zbase
                                , float x
                                , float z
                                , Brush const& brush
                                , float change
                                , scoped_blp_texture_reference const& texture_to_replace
                                , scoped_blp_texture_reference replacement_texture
                                )
{
  float dist = misc::getShortestDist(x, z, xbase, zbase, CHUNKSIZE);
  float radius = brush.get_radius();

  if (dist > radius)
  {
    return false;
  }

  // if the chunk is fully inside the brush, just swap the 2 textures
  if (misc::square_is_in_circle(x, z, radius, xbase, zbase, CHUNKSIZE))
  {
    replace_texture(texture_to_replace, std::move (replacement_texture));
    return true;
  }

  bool changed = false;
  int old_tex_level = -1, new_tex_level = -1;
  float x_pos, z_pos = zbase;

  for (int i=0; i<nTextures; ++i)
  {
    if (_textures[i] == texture_to_replace->filename)
    {
      old_tex_level = i;
    }
    if (_textures[i] == replacement_texture->filename)
    {
      new_tex_level = i;
    }
  }

  if (old_tex_level == -1 || (new_tex_level == -1 && nTextures == 4 && !eraseUnusedTextures()))
  {
    return false;
  }

  if (new_tex_level == -1)
  {
    new_tex_level = addTexture(std::move (replacement_texture));
  }

  if (old_tex_level == new_tex_level)
  {
    return false;
  }

  create_temporary_alphamaps_if_needed();
  auto& amap = *tmp_edit_values.get();

  for (int j = 0; j < 64; j++)
  {
    x_pos = xbase;
    for (int i = 0; i < 64; ++i)
    {
      dist = misc::dist(x, z, x_pos + TEXDETAILSIZE / 2.0f, z_pos + TEXDETAILSIZE / 2.0f);

      if (dist <= radius)
      {
        int offset = j * 64 + i;

        float v = amap[old_tex_level][offset] * brush.value_at_dist(dist) * change;
        amap[new_tex_level][offset] += v;
        amap[old_tex_level][offset] -= v;

        changed = true;
      }

      x_pos += TEXDETAILSIZE;
    }

    z_pos += TEXDETAILSIZE;
  }

  if (changed)
  {
    require_update();
  }

  return changed;
}

unsigned int TextureSet::flag(size_t id)
{
  return _layers_info[id].flags;
}

void TextureSet::setEffect(size_t id, int value)
{
  _layers_info[id].effectID = value;
}

unsigned int TextureSet::effect(size_t id)
{
  return _layers_info[id].effectID;
}

bool TextureSet::is_animated(std::size_t id) const
{
  return (id < nTextures ? (_layers_info[id].flags & FLAG_ANIMATE) : false);
}

void TextureSet::change_texture_flag(scoped_blp_texture_reference const& tex, std::size_t flag, bool add)
{
  for (size_t i = 0; i < nTextures; ++i)
  {
    if (_textures[i] == tex->filename)
    {
      if (add)
      {
        // override the current speed/rotation
        if (flag & 0x3F)
        {
          _layers_info[i].flags &= ~0x3F;
        }
        _layers_info[i].flags |= flag;
      }
      else
      {
        _layers_info[i].flags &= ~flag;
      }

      if (flag & FLAG_GLOW)
      {
        _layers_info[i].flags |= FLAG_GLOW;
      }
      else
      {
        _layers_info[i].flags &= ~FLAG_GLOW;
      }

      break;
    }
  }
}
void TextureSet::clear_texture_flags()
{
  for (size_t i = 0; i < nTextures; ++i)
  {
    _layers_info[i].flags = 0;
  }
}

std::vector<std::vector<uint8_t>> TextureSet::save_alpha(bool big_alphamap)
{
  std::vector<std::vector<uint8_t>> amaps;

  apply_alpha_changes();

  if (nTextures > 1)
  {
    if (big_alphamap)
    {
      for (int i = 0; i < nTextures - 1; ++i)
      {
        const uint8_t* alphamap = alphamaps[i]->getAlpha();
        amaps.emplace_back(alphamap, alphamap + 4096);
      }
    }
    else
    {
      uint8_t tab[4096 * 3];

      if (_do_not_convert_alphamaps)
      {
        for (size_t k = 0; k < nTextures - 1; k++)
        {
          memcpy(tab + (k*64*64), alphamaps[k]->getAlpha(), 64 * 64);
        }
      }
      else
      {
        alphas_to_old_alpha(tab);
      }


      auto const combine_nibble
      (
        [&] (int layer, int pos)
        {
          int index = layer * 4096 + pos * 2;
          return ((tab[index] & 0xF0) >> 4) | (tab[index + 1] & 0xF0);
        }
      );

      for (size_t layer = 0; layer < nTextures - 1; ++layer)
      {
        amaps.emplace_back(2048);
        auto& layer_data = amaps.back();

        for (int i = 0; i < 2048; ++i)
        {
          layer_data[i] = combine_nibble(layer, i);
        }
      }
    }
  }

  return amaps;
}

// dest = tab [4096 * (nTextures - 1)]
// call only if nTextures > 1
void TextureSet::alphas_to_big_alpha(uint8_t* dest)
{
  auto alpha
  (
    [&] (int layer, int pos = 0)
    {
      return dest + layer * 4096 + pos;
    }
  );

  for (size_t k = 0; k < nTextures - 1; k++)
  {
    memcpy(alpha(k), alphamaps[k]->getAlpha(), 4096);
  }

  for (int i = 0; i < 64 * 64; ++i)
  {
    int a = 255;

    for (int k = nTextures - 2; k >= 0; --k)
    {
      uint8_t* value_ptr = alpha(k, i);
      uint8_t val = alpha_convertion_lookup[*value_ptr * a];
      a -= val;
      *value_ptr = val;
    }
  }
}

void TextureSet::convertToBigAlpha()
{
  // nothing to do
  if (nTextures < 2)
  {
    return;
  }

  std::array<std::uint8_t, 4096 * 3> tab;

  apply_alpha_changes();
  alphas_to_big_alpha(tab.data());

  for (size_t k = 0; k < nTextures - 1; k++)
  {
    alphamaps[k]->setAlpha(tab.data() + 4096 * k);
  }

  require_update();
}

// dest = tab [4096 * (nTextures - 1)]
// call only if nTextures > 1
void TextureSet::alphas_to_old_alpha(uint8_t* dest)
{
  auto alpha
  (
    [&] (int layer, int pos = 0)
    {
      return dest + layer * 4096 + pos;
    }
  );

  for (size_t k = 0; k < nTextures - 1; k++)
  {
    memcpy(alpha(k), alphamaps[k]->getAlpha(), 64 * 64);
  }

  for (int i = 0; i < 64 * 64; ++i)
  {
    // a = remaining visibility
    int a = 255;

    for (int k = nTextures - 2; k >= 0; --k)
    {
      if (a <= 0)
      {
        *alpha(k, i) = 0;
      }
      else
      {
        int current = *alpha(k, i);
        // convert big alpha value to old alpha
        *alpha(k, i) = misc::rounded_int_div(current * 255, a);
        // remove big alpha value from the remaining visibility
        a -= current;
      }
    }
  }
}

void TextureSet::convertToOldAlpha()
{
  // nothing to do
  if (nTextures < 2)
  {
    return;
  }

  uint8_t tab[3 * 4096];

  apply_alpha_changes();
  alphas_to_old_alpha(tab);

  for (size_t k = 0; k < nTextures - 1; k++)
  {
    alphamaps[k]->setAlpha(tab + k * 4096);
  }

  require_update();
}

void TextureSet::merge_layers(size_t id1, size_t id2)
{
  if (id1 >= nTextures || id2 >= nTextures || id1 == id2)
  {
    throw std::invalid_argument("merge_layers: invalid layer id(s)");
  }

  if (id2 < id1)
  {
    std::swap(id2, id1);
  }

  create_temporary_alphamaps_if_needed();

  auto& amap = *tmp_edit_values.get();

  for (int i = 0; i < 64 * 64; ++i)
  {
    amap[id1][i] += amap[id2][i];
    // no need to set the id alphamap to 0, it'll be done in "eraseTexture(id2)"
  }

  eraseTexture(id2);
  require_update();
}

bool TextureSet::removeDuplicate()
{
  bool changed = apply_alpha_changes();

  for (size_t i = 0; i < nTextures; i++)
  {
    for (size_t j = i + 1; j < nTextures; j++)
    {
      if (_textures[i] == _textures[j])
      {
        merge_layers(i, j);
        changed = true;
        j--; // otherwise it skips the next texture
      }
    }
  }

  return changed;
}

namespace
{
  misc::max_capacity_stack_vector<std::size_t, 4> current_layer_values
    (std::uint8_t nTextures, std::unique_ptr<Alphamap> const* alphamaps, std::size_t pz, std::size_t px)
  {
    misc::max_capacity_stack_vector<std::size_t, 4> values (nTextures, 0xFF);
    for (std::uint8_t i = 1; i < nTextures; ++i)
    {
      values[i] = alphamaps[i - 1].get()->getAlpha(64 * pz + px);
      values[0] -= values[i];
    }
    return values;
  }
}

std::vector<uint8_t> TextureSet::lod_texture_map()
{
  // make sure all changes have been applied
  apply_alpha_changes();

  if (_need_lod_texture_map_update)
  {
    update_lod_texture_map();
  }

  return _lod_texture_map;
}

void TextureSet::update_lod_texture_map()
{
  std::vector<std::uint8_t> lod;

  for (std::size_t z = 0; z < 8; ++z)
  {
    for (std::size_t x = 0; x < 8; ++x)
    {
      misc::max_capacity_stack_vector<std::size_t, 4> dominant_square_count (nTextures);

      for (std::size_t pz = z * 8; pz < (z + 1) * 8; ++pz)
      {
        for (std::size_t px = x * 8; px < (x + 1) * 8; ++px)
        {
          ++dominant_square_count[max_element_index (current_layer_values (nTextures, alphamaps.data(), pz, px))];
        }
      }
      lod.push_back (max_element_index (dominant_square_count));
    }
  }

  _lod_texture_map = lod;
  _need_lod_texture_map_update = false;
}

uint8_t TextureSet::sum_alpha(size_t offset) const
{
  uint8_t sum = 0;

  for (auto const& amap : alphamaps)
  {
    if (amap)
    {
      sum += amap->getAlpha(offset);
    }
  }

  return sum;
}

namespace
{
  inline std::uint8_t float_alpha_to_uint8(float a)
  {
    return static_cast<std::uint8_t>(std::max(0.f, std::min(255.f, std::round(a))));
  }
}

bool TextureSet::apply_alpha_changes()
{
  if (!tmp_edit_values || nTextures < 2)
  {
    tmp_edit_values.reset();
    return false;
  }

  auto& new_amaps = *tmp_edit_values.get();
  std::array<std::uint16_t, 64 * 64> totals;
  totals.fill(0);

  for (int alpha_layer = 0; alpha_layer < nTextures - 1; ++alpha_layer)
  {
    std::array<std::uint8_t, 64 * 64> values;

    for (int i = 0; i < 64 * 64; ++i)
    {
      values[i] = float_alpha_to_uint8(new_amaps[alpha_layer + 1][i]);
      totals[i] += values[i];

      // remove the possible overflow with rounding
      // max 2 if all 4 values round up so it won't change the layer's alpha much
      if (totals[i] > 255)
      {
        values[i] -= static_cast<std::uint8_t>(totals[i] - 255);
      }
    }

    alphamaps[alpha_layer]->setAlpha(values.data());
  }

  require_update();

  tmp_edit_values.reset();

  return true;
}

void TextureSet::create_temporary_alphamaps_if_needed()
{
  if (tmp_edit_values || nTextures < 2)
  {
    return;
  }

  tmp_edit_values = std::make_unique<tmp_edit_alpha_values>();

  tmp_edit_alpha_values& values = *tmp_edit_values.get();

  for (int i = 0; i < 64 * 64; ++i)
  {
    float base_alpha = 255.f;

    for (int alpha_layer = 0; alpha_layer < nTextures - 1; ++alpha_layer)
    {
      float f = static_cast<float>(alphamaps[alpha_layer]->getAlpha(i));

      values[alpha_layer + 1][i] = f;
      base_alpha -= f;
    }

    values[0][i] = base_alpha;
  }
}

void TextureSet::update_alpha_shadow_map_if_needed(int chunk_x, int chunk_y, chunk_shadow* shadow, noggit::chunk_data* preview_data)
{
  if (_need_amap_update)
  {
    opengl::texture::set_active_texture(0);

    bool use_shadow = (shadow != nullptr);

    if (preview_data)
    {
      std::vector<uint8_t> amap(4 * 64 * 64);
      uint8_t const* alpha_ptr[3];
      int preview_alpha_count = preview_data->texture_count - 1;

      for (int i = 0; i < preview_alpha_count; ++i)
      {
        alpha_ptr[i] = preview_data->alphamaps[i].getAlpha();
      }

      for (int i = 0; i < 64 * 64; ++i)
      {
        for (int alpha_id = 0; alpha_id < 3; ++alpha_id)
        {
          amap[i * 4 + alpha_id] = (alpha_id < preview_alpha_count)
            ? *(alpha_ptr[alpha_id]++)
            : 0
            ;
        }

        amap[i * 4 + 3] = use_shadow ? ((shadow->data[i / 64] >> (i % 64)) & 1) * 255 : 0;
      }

      gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, chunk_x + 16 * chunk_y, 64, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, amap.data());
    }
    else if (nTextures)
    {
      if (_first_amap_setup)
      {
        gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, chunk_x + 16 * chunk_y, 64, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, _setup_amap_data->data());
        _first_amap_setup = false;
        _setup_amap_data.reset();
      }
      else if (tmp_edit_values)
      {
        std::vector<float> amap(4 * 64 * 64);
        auto& tmp_amaps = *tmp_edit_values.get();

        for (int i = 0; i < 64 * 64; ++i)
        {
          for (int alpha_id = 0; alpha_id < 3; ++alpha_id)
          {
            amap[i * 4 + alpha_id] = tmp_amaps[alpha_id + 1][i] / 255.f;
          }

          amap[i * 4 + 3] = use_shadow ? (shadow->data[i / 64] >> (i % 64)) & 1 : 0;
        }

        gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, chunk_x + 16 * chunk_y, 64, 64, 1, GL_RGBA, GL_FLOAT, amap.data());
      }
      else
      {
        std::vector<uint8_t> amap(4 * 64 * 64);
        uint8_t const* alpha_ptr[3];

        for (int i = 0; i < nTextures - 1; ++i)
        {
          alpha_ptr[i] = alphamaps[i]->getAlpha();
        }

        for (int i = 0; i < 64 * 64; ++i)
        {
          for (int alpha_id = 0; alpha_id < 3; ++alpha_id)
          {
            amap[i * 4 + alpha_id] = (alpha_id < nTextures - 1)
              ? *(alpha_ptr[alpha_id]++)
              : 0
              ;
          }

          amap[i * 4 + 3] = use_shadow ? ((shadow->data[i / 64] >> (i % 64)) & 1) * 255 : 0;
        }

        gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, chunk_x + 16 * chunk_y, 64, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, amap.data());
      }
    }

    _need_amap_update = false;
  }
}

std::array<std::uint8_t, 256 * 256> TextureSet::alpha_convertion_lookup = TextureSet::make_alpha_lookup_array();

void TextureSet::require_update()
{
  _need_amap_update = true;
  _need_lod_texture_map_update = true;
  // only used when first setting up the alphamap with pre computed value
  // from the async loading, need to be cleared for adt using the 1x1 alphamap
  // as they never have to setup the alphamap from the texture set
  _first_amap_setup = false;
}
