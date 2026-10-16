// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/ModelInstance.h> // ModelInstance
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/client_profile.hpp>
#include <noggit/liquid_tile.hpp>
#include <noggit/settings.hpp>
#include <noggit/WMOInstance.h> // WMOInstance
#include <noggit/World.h>
#include <noggit/alphamap.hpp>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/tileset_array_handler.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <util/sExtendableArray.hpp>

#include <algorithm>
#include <cassert>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

MapTile::MapTile( int pX
                , int pZ
                , std::string const& pFilename
                , bool pBigAlpha
                , bool pLoadModels
                , bool use_mclq_green_lava
                , bool reloading_tile
                , World* world
                , tile_mode mode
                )
  : AsyncObject(pFilename)
  , index(tile_index(pX, pZ))
  , xbase(pX * TILESIZE)
  , zbase(pZ * TILESIZE)
  , changed(false)
  , Water (this, xbase, zbase, use_mclq_green_lava)
  , _mode(mode)
  , _tile_is_being_reloaded(reloading_tile)
  , mBigAlpha(pBigAlpha)
  , _load_models(pLoadModels)
  , _world(world)
{
}

MapTile::~MapTile()
{
  _world->remove_models_if_needed(uids);
}

void MapTile::finishLoading()
{
  MPQFile theFile(filename);

  NOGGIT_LOG << "Opening tile " << index.x << ", " << index.z << " (\"" << filename << "\") from " << (theFile.isExternal() ? "disk" : "MPQ") << "." << std::endl;

  // - Parsing the file itself. --------------------------

  // We store this data to load it at the end.
  uint32_t lMCNKOffsets[256];
  std::vector<ENTRY_MDDF> lModelInstances;
  std::vector<ENTRY_MODF> lWMOInstances;

  uint32_t fourcc;
  uint32_t size;

  MHDR Header;

  // - MVER ----------------------------------------------

  uint32_t version;

  theFile.read(&fourcc, 4);
  theFile.seekRelative(4);
  theFile.read(&version, 4);

  assert(fourcc == 'MVER' && version == 18);

  // - MHDR ----------------------------------------------

  theFile.read(&fourcc, 4);
  theFile.seekRelative(4);

  assert(fourcc == 'MHDR');

  theFile.read(&Header, sizeof(MHDR));

  mFlags = Header.flags;

  // - MCIN ----------------------------------------------

  theFile.seek(Header.mcin + 0x14);
  theFile.read(&fourcc, 4);
  theFile.seekRelative(4);

  assert(fourcc == 'MCIN');

  for (int i = 0; i < 256; ++i)
  {
    theFile.read(&lMCNKOffsets[i], 4);
    theFile.seekRelative(0xC);
  }

  // - MTEX ----------------------------------------------

  theFile.seek(Header.mtex + 0x14);
  theFile.read(&fourcc, 4);
  theFile.read(&size, 4);

  assert(fourcc == 'MTEX');

  {
    char const* lCurPos = reinterpret_cast<char const*>(theFile.getPointer());
    char const* lEnd = lCurPos + size;

    while (lCurPos < lEnd)
    {
      mTextureFilenames.push_back(noggit::mpq::normalized_filename(std::string(lCurPos)));
      lCurPos += strlen(lCurPos) + 1;
    }
  }

  if (_load_models)
  {
    // - MMDX ----------------------------------------------

    theFile.seek(Header.mmdx + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MMDX');

    {
      char const* lCurPos = reinterpret_cast<char const*>(theFile.getPointer());
      char const* lEnd = lCurPos + size;

      while (lCurPos < lEnd)
      {
        mModelFilenames.push_back(noggit::mpq::normalized_filename(std::string(lCurPos)));
        lCurPos += strlen(lCurPos) + 1;
      }
    }

    // - MWMO ----------------------------------------------

    theFile.seek(Header.mwmo + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MWMO');

    {
      char const* lCurPos = reinterpret_cast<char const*>(theFile.getPointer());
      char const* lEnd = lCurPos + size;

      while (lCurPos < lEnd)
      {
        mWMOFilenames.push_back(noggit::mpq::normalized_filename(std::string(lCurPos)));
        lCurPos += strlen(lCurPos) + 1;
      }
    }

    // - MDDF ----------------------------------------------

    theFile.seek(Header.mddf + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MDDF');

    ENTRY_MDDF const* mddf_ptr = reinterpret_cast<ENTRY_MDDF const*>(theFile.getPointer());
    for (unsigned int i = 0; i < size / sizeof(ENTRY_MDDF); ++i)
    {
      lModelInstances.push_back(mddf_ptr[i]);
    }

    // - MODF ----------------------------------------------

    theFile.seek(Header.modf + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MODF');

    ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(theFile.getPointer());
    for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
    {
      lWMOInstances.push_back(modf_ptr[i]);
    }
  }

  // - MISC ----------------------------------------------

  //! \todo  Parse all chunks in the new style!

  // - MH2O ----------------------------------------------
  if (Header.mh2o != 0) {
    theFile.seek(Header.mh2o + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    int ofsW = Header.mh2o + 0x14 + 0x8;
    assert(fourcc == 'MH2O');

    Water.readFromFile(theFile, ofsW);
  }

  // - MFBO ----------------------------------------------

  if (mFlags & 1)
  {
    theFile.seek(Header.mfbo + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MFBO');

    int16_t mMaximum[9], mMinimum[9];
    theFile.read(mMaximum, sizeof(mMaximum));
    theFile.read(mMinimum, sizeof(mMinimum));

    const float xPositions[] = { this->xbase, this->xbase + 266.0f, this->xbase + 533.0f };
    const float yPositions[] = { this->zbase, this->zbase + 266.0f, this->zbase + 533.0f };

    for (int y = 0; y < 3; y++)
    {
      for (int x = 0; x < 3; x++)
      {
        int pos = x + y * 3;
        // fix bug with old noggit version inverting values
        auto&& z{ std::minmax (mMinimum[pos], mMaximum[pos]) };

        mMinimumValues[pos] = { xPositions[x], static_cast<float>(z.first), yPositions[y] };
        mMaximumValues[pos] = { xPositions[x], static_cast<float>(z.second), yPositions[y] };
      }
    }
  }

  // - MTXF ----------------------------------------------
  if (Header.mtxf != 0)
  {
    theFile.seek(Header.mtxf + 0x14);

    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MTXF');

    int count = size / 0x4;

    std::vector<mtxf_entry> mtxf_data(count);

    theFile.read(mtxf_data.data(), size);

    for (int i = 0; i < count; ++i)
    {
      _mtxf_entries[mTextureFilenames[i]] = mtxf_data[i];
    }
  }

  // - Done. ---------------------------------------------

  // - Load textures -------------------------------------

  //! \note We no longer pre load textures but the chunks themselves do.

  if (_load_models)
  {
    // - Load WMOs -----------------------------------------

    for (auto const& object : lWMOInstances)
    {
      add_model(_world->add_wmo_instance(WMOInstance(mWMOFilenames[object.nameID], &object), _tile_is_being_reloaded));
    }

    // - Load M2s ------------------------------------------

    for (auto const& model : lModelInstances)
    {
      add_model(_world->add_model_instance(ModelInstance(mModelFilenames[model.nameID], &model), _tile_is_being_reloaded));
    }

    _world->need_model_updates = true;
  }

  // - Load chunks ---------------------------------------

  for (int nextChunk = 0; nextChunk < 256; ++nextChunk)
  {
    theFile.seek(lMCNKOffsets[nextChunk]);
    mChunks[nextChunk / 16][nextChunk % 16] = std::make_unique<MapChunk> (this, &theFile, mBigAlpha, _mode);
  }

  theFile.close();

  // no or one texture only means we don't need to generate an alphamap bigger than 1x1 per layer
  if (mTextureFilenames.size() <= 1)
  {
    _use_no_alpha_alphamap = true;
  }

  // otherwise lods have visual issues from underground vertices being at "max" depth
  Water.update_underground_vertices_depth();

  // - Really done. --------------------------------------

  LogDebug << "Done loading tile " << index.x << "," << index.z << "." << std::endl;
  finished = true;
  _tile_is_being_reloaded = false;
  _state_changed.notify_all();
}

bool MapTile::isTile(int pX, int pZ)
{
  return pX == index.x && pZ == index.z;
}

void MapTile::convert_alphamap(bool to_big_alpha)
{
  if (mBigAlpha != to_big_alpha)
  {
    mBigAlpha = to_big_alpha;
    // force alphamap update
    _alphamap_created = false;

    for (size_t i = 0; i < 16; i++)
    {
      for (size_t j = 0; j < 16; j++)
      {
        mChunks[i][j]->use_big_alphamap = to_big_alpha;
      }
    }
  }
}

void MapTile::draw ( math::frustum const& frustum
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
                   )
{
  if (!finished)
  {
    return;
  }

  if (_need_recalc_extents)
  {
    recalc_extents();
  }

  if (need_visibility_update || _need_visibility_update)
  {
    update_visibility(cull_distance, frustum, camera, display);
    _need_chunk_data_update = true;
  }

  if (!_is_visible)
  {
    return;
  }

  if (!_alphamap_created)
  {
    create_combined_alpha_shadow_map();

    if (!_uploaded)
    {
      upload();

      // make sure all the textures are in the array
      for (std::string const& tex : mTextureFilenames)
      {
        tileset_handler.get_texture_position(tex);
      }

      tileset_handler.bind();

      _ubo.upload();
      gl.bindBuffer(GL_UNIFORM_BUFFER, _chunks_data_ubo);
      gl.bufferData(GL_UNIFORM_BUFFER, sizeof(chunk_shader_data) * 256, NULL, GL_STATIC_DRAW);
      gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

      opengl::scoped::vao_binder const _(_vao);

      mcnk_shader.attrib(_, "position", _vertices_vbo, 3, GL_FLOAT, GL_FALSE, sizeof(chunk_vertex), static_cast<char*>(0) + offsetof(chunk_vertex, position));
      mcnk_shader.attrib(_, "normal", _vertices_vbo, 3, GL_FLOAT, GL_FALSE, sizeof(chunk_vertex), static_cast<char*>(0) + offsetof(chunk_vertex, normal));
      mcnk_shader.attrib(_, "mccv", _vertices_vbo, 3, GL_FLOAT, GL_FALSE, sizeof(chunk_vertex), static_cast<char*>(0) + offsetof(chunk_vertex, color));
      mcnk_shader.attrib(_, "texcoord", tex_coord_vbo, 2, GL_FLOAT, GL_FALSE, 0, 0);

      need_visibility_update = true;

      // need to set it back after loading tilesets
      opengl::texture::set_active_texture(0);
    }

    _need_chunk_data_update = true;
  }

  _adt_alphamap.bind();

  gl.bindBufferBase(GL_UNIFORM_BUFFER, 0, _chunks_data_ubo);
  gl.bindVertexArray(_vao);

  if (_need_chunk_data_update || selected_texture_changed)
  {
    gl.bindBuffer(GL_ARRAY_BUFFER, _vertices_vbo);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indices_vbo);

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        mChunks[z][x]->prepare_draw( camera
                                   , need_visibility_update
                                   , selected_texture_changed
                                   , current_texture
                                   , area_id_colors
                                   , display
                                   , tileset_handler
                                   , _indices_offsets
                                   , _indices_count
                                   );
      }
    }

    _need_chunk_data_update = false;
  }

  gl.multiDrawElements(GL_TRIANGLES, _indices_count.data(), GL_UNSIGNED_SHORT, _indices_offsets.data(), 256);
}

void MapTile::draw_shadows(opengl::scoped::use_program& shadow_shader)
{
  if (!finished)
  {
    return;
  }

  opengl::scoped::vao_binder const _ (_shadow_vao);
  gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indices_vbo);

  shadow_shader.attrib(_, "position", _vertices_vbo, 3, GL_FLOAT, GL_FALSE, sizeof(chunk_vertex), static_cast<char*>(0) + offsetof(chunk_vertex, position));
  shadow_shader.attrib(_, "normal", _vertices_vbo, 3, GL_FLOAT, GL_FALSE, sizeof(chunk_vertex), static_cast<char*>(0) + offsetof(chunk_vertex, normal));

  gl.multiDrawElements(GL_TRIANGLES, _indices_count.data(), GL_UNSIGNED_SHORT, _indices_offsets.data(), 256);
}

void MapTile::intersect (math::ray const& ray, selection_result* results, bool ignore_terrain_holes)
{
  if (!finished)
  {
    return;
  }

  if (_need_recalc_extents)
  {
    recalc_extents();
  }

  if (!ray.intersect_bounds(_extents[0], _extents[1]))
  {
    return;
  }

  for (size_t j (0); j < 16; ++j)
  {
    for (size_t i (0); i < 16; ++i)
    {
      mChunks[j][i]->intersect (ray, results, ignore_terrain_holes);
    }
  }
}
void MapTile::intersect_liquids (math::ray const& ray, selection_result* results)
{
  if (!finished)
  {
    return;
  }

  Water.intersect(ray, results);
}


void MapTile::drawMFBO (opengl::scoped::use_program& mfbo_shader)
{
  static std::vector<std::uint8_t> const indices = {4, 1, 2, 5, 8, 7, 6, 3, 0, 1, 0, 3, 6, 7, 8, 5, 2, 1};

  if (!_mfbo_buffer_are_setup)
  {
    _mfbo_vbos.upload();
    _mfbo_vaos.upload();



    gl.bufferData<GL_ARRAY_BUFFER>( _mfbo_bottom_vbo
                                  , 9 * sizeof(math::vector_3d)
                                  , mMinimumValues
                                  , GL_STATIC_DRAW
                                  );
    gl.bufferData<GL_ARRAY_BUFFER>( _mfbo_top_vbo
                                  , 9 * sizeof(math::vector_3d)
                                  , mMaximumValues
                                  , GL_STATIC_DRAW
                                  );

    {
      opengl::scoped::vao_binder const _ (_mfbo_bottom_vao);
      mfbo_shader.attrib(_, "position", _mfbo_bottom_vbo, 3, GL_FLOAT, GL_FALSE, 0, 0);
    }

    {
      opengl::scoped::vao_binder const _(_mfbo_top_vao);
      mfbo_shader.attrib(_, "position", _mfbo_top_vbo, 3, GL_FLOAT, GL_FALSE, 0, 0);
    }

    _mfbo_buffer_are_setup = true;
  }

  {
    opengl::scoped::vao_binder const _(_mfbo_bottom_vao);
    mfbo_shader.uniform("color", math::vector_4d(1.0f, 1.0f, 0.0f, 0.2f));
    gl.drawElements(GL_TRIANGLE_FAN, indices.size(), indices);
  }

  {
    opengl::scoped::vao_binder const _(_mfbo_top_vao);
    mfbo_shader.uniform("color", math::vector_4d(0.0f, 1.0f, 1.0f, 0.2f));
    gl.drawElements(GL_TRIANGLE_FAN, indices.size(), indices);
  }
}

void MapTile::drawWater ( math::frustum const& frustum
                        , const float& cull_distance
                        , const math::vector_3d& camera
                        , bool camera_moved
                        , liquid_render& render
                        , opengl::scoped::use_program& water_shader
                        , int animtime
                        , int layer
                        , display_mode display
                        )
{
  if (!Water.has_water())
  {
    return; //no need to draw water on tile without water =)
  }

  Water.draw ( frustum
             , cull_distance
             , camera
             , camera_moved
             , render
             , water_shader
             , animtime
             , layer
             , display
             );
}

MapChunk* MapTile::getChunk(unsigned int x, unsigned int z)
{
  if (x < 16 && z < 16)
  {
    return mChunks[z][x].get();
  }
  else
  {
    return nullptr;
  }
}

std::vector<MapChunk*> MapTile::chunks_in_range (math::vector_3d const& pos, float radius) const
{
  std::vector<MapChunk*> chunks;

  for (size_t ty (0); ty < 16; ++ty)
  {
    for (size_t tx (0); tx < 16; ++tx)
    {
      if (misc::getShortestDist (pos.x, pos.z, mChunks[ty][tx]->xbase, mChunks[ty][tx]->zbase, CHUNKSIZE) <= radius)
      {
        chunks.emplace_back (mChunks[ty][tx].get());
      }
    }
  }

  return chunks;
}

std::vector<MapChunk*> MapTile::chunks_between (math::vector_3d const& pos1, math::vector_3d const& pos2) const
{
  std::vector<MapChunk*> chunks;

  for (size_t ty (0); ty < 16; ++ty)
  {
    for (size_t tx (0); tx < 16; ++tx)
    {
      auto minX = mChunks[ty][tx]->xbase;
      auto minZ = mChunks[ty][tx]->zbase;
      auto maxX = minX+CHUNKSIZE;
      auto maxZ = minZ+CHUNKSIZE;
      if(minX <= pos2.x && maxX >= pos1.x &&
        minZ <= pos2.z && maxZ >= pos1.z)
        {
          chunks.emplace_back(mChunks[ty][tx].get());
        }
    }
  }

  return chunks;
}

bool MapTile::GetVertex(float x, float z, math::vector_3d *V)
{
  int xcol = (int)((x - xbase) / CHUNKSIZE);
  int ycol = (int)((z - zbase) / CHUNKSIZE);

  return xcol >= 0 && xcol <= 15 && ycol >= 0 && ycol <= 15 && mChunks[ycol][xcol]->GetVertex(x, z, V);
}

/// --- Only saving related below this line. --------------------------

void MapTile::saveTile(World* world)
{
  // WoW 1.12.1 (the only supported client) reads liquid from the per-chunk
  // MCLQ, so the MCLQ variant is the only one that is ever saved. The
  // upstream two-pass save wrote an MH2O-only file first and the MCLQ file
  // second; whenever the second pass was skipped (setting off or invalid)
  // the saved file silently lost all water data
  save(world);
}

void MapTile::save(World* world)
{
  NOGGIT_LOG << "Saving ADT \"" << filename << "\"." << std::endl;

  int lID;  // This is a global counting variable. Do not store something in here you need later.
  std::vector<WMOInstance> lObjectInstances;
  std::vector<ModelInstance> lModelInstances;

  // Check which doodads and WMOs are on this ADT.
  math::vector_3d lTileExtents[2];
  lTileExtents[0] = math::vector_3d(xbase, 0.0f, zbase);
  lTileExtents[1] = math::vector_3d(xbase + TILESIZE, 0.0f, zbase + TILESIZE);

  // get every models on the tile
  for (std::uint32_t uid : uids)
  {
    auto model = world->get_model(uid);

    if (!model)
    {
      // todo: save elsewhere if this happens ? it shouldn't but still
      LogError << "Could not fine model with uid=" << uid << " when saving " << filename << std::endl;
    }
    else
    {
      if (model.value().index() == eEntry_WMO)
      {
        lObjectInstances.emplace_back(*std::get<selected_wmo_type>(model.value()));
      }
      else
      {
        lModelInstances.emplace_back(*std::get<selected_model_type>(model.value()));
      }
    }
  }

  struct filenameOffsetThing
  {
    int nameID;
    int filenamePosition;
  };

  filenameOffsetThing nullyThing = { 0, 0 };

  std::map<std::string, filenameOffsetThing> lModels;

  for (auto const& model : lModelInstances)
  {
    if (lModels.find(model.model->filename) == lModels.end())
    {
      lModels.emplace (model.model->filename, nullyThing);
    }
  }

  lID = 0;
  for (auto& model : lModels)
  {
    model.second.nameID = lID++;
  }

  std::map<std::string, filenameOffsetThing> lObjects;

  for (auto const& object : lObjectInstances)
  {
    if (lObjects.find(object.wmo->filename) == lObjects.end())
    {
      lObjects.emplace (object.wmo->filename, nullyThing);
    }
  }

  lID = 0;
  for (auto& object : lObjects)
  {
    object.second.nameID = lID++;
  }

  // Check which textures are on this ADT.
  std::map<std::string, int> lTextures;

  for (int i = 0; i < 16; ++i)
  {
    for (int j = 0; j < 16; ++j)
    {
      for (size_t tex = 0; tex < mChunks[i][j]->texture_set->num(); tex++)
      {
        if (lTextures.find(mChunks[i][j]->texture_set->texture(tex)) == lTextures.end())
        {
          lTextures.emplace(mChunks[i][j]->texture_set->texture(tex), -1);
        }
      }
    }
  }

  lID = 0;
  for (auto& texture : lTextures)
    texture.second = lID++;

  // Now write the file.
  util::sExtendableArray lADTFile;

  int lCurrentPosition = 0;

  // MVER
  lADTFile.Extend(8 + 0x4);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MVER', 4);

  // MVER data
  *(lADTFile.GetPointer<int>(8)) = 18;
  lCurrentPosition += 8 + 0x4;

  // MHDR
  int lMHDR_Position = lCurrentPosition;
  lADTFile.Extend(8 + 0x40);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MHDR', 0x40);

  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->flags = mFlags;

  lCurrentPosition += 8 + 0x40;


  // MCIN
  int lMCIN_Position = lCurrentPosition;

  lADTFile.Extend(8 + 256 * 0x10);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MCIN', 256 * 0x10);
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mcin = lCurrentPosition - 0x14;

  lCurrentPosition += 8 + 256 * 0x10;

  // MTEX
  int lMTEX_Position = lCurrentPosition;
  lADTFile.Extend(8 + 0);  // We don't yet know how big this will be.
  SetChunkHeader(lADTFile, lCurrentPosition, 'MTEX');
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mtex = lCurrentPosition - 0x14;

  lCurrentPosition += 8 + 0;

  // MTEX data
  for (auto const& texture : lTextures)
  {
    lADTFile.Insert(lCurrentPosition, texture.first.size() + 1, texture.first.c_str());

    lCurrentPosition += texture.first.size() + 1;
    lADTFile.GetPointer<sChunkHeader>(lMTEX_Position)->mSize += texture.first.size() + 1;
    LogDebug << "Added texture \"" << texture.first << "\"." << std::endl;
  }

  // MMDX
  int lMMDX_Position = lCurrentPosition;
  lADTFile.Extend(8 + 0);  // We don't yet know how big this will be.
  SetChunkHeader(lADTFile, lCurrentPosition, 'MMDX');
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mmdx = lCurrentPosition - 0x14;

  lCurrentPosition += 8 + 0;

  // MMDX data
  for (auto it = lModels.begin(); it != lModels.end(); ++it)
  {
    it->second.filenamePosition = lADTFile.GetPointer<sChunkHeader>(lMMDX_Position)->mSize;
    lADTFile.Insert(lCurrentPosition, it->first.size() + 1, misc::normalize_adt_filename(it->first).c_str());
    lCurrentPosition += it->first.size() + 1;
    lADTFile.GetPointer<sChunkHeader>(lMMDX_Position)->mSize += it->first.size() + 1;
    LogDebug << "Added model \"" << it->first << "\"." << std::endl;
  }

  // MMID
  // M2 model names
  int lMMID_Size = 4 * lModels.size();
  lADTFile.Extend(8 + lMMID_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MMID', lMMID_Size);
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mmid = lCurrentPosition - 0x14;

  // MMID data
  // WMO model names
  auto const lMMID_Data = lADTFile.GetPointer<int>(lCurrentPosition + 8);

  lID = 0;
  for (auto const& model : lModels)
  {
    lMMID_Data[lID] = model.second.filenamePosition;
    lID++;
  }
  lCurrentPosition += 8 + lMMID_Size;

  // MWMO
  int lMWMO_Position = lCurrentPosition;
  lADTFile.Extend(8 + 0);  // We don't yet know how big this will be.
  SetChunkHeader(lADTFile, lCurrentPosition, 'MWMO');
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mwmo = lCurrentPosition - 0x14;

  lCurrentPosition += 8 + 0;

  // MWMO data
  for (auto& object : lObjects)
  {
    object.second.filenamePosition = lADTFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize;
    lADTFile.Insert(lCurrentPosition, object.first.size() + 1, misc::normalize_adt_filename(object.first).c_str());
    lCurrentPosition += object.first.size() + 1;
    lADTFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize += object.first.size() + 1;
    LogDebug << "Added object \"" << object.first << "\"." << std::endl;
  }

  // MWID
  int lMWID_Size = 4 * lObjects.size();
  lADTFile.Extend(8 + lMWID_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MWID', lMWID_Size);
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mwid = lCurrentPosition - 0x14;

  // MWID data
  auto const lMWID_Data = lADTFile.GetPointer<int>(lCurrentPosition + 8);

  lID = 0;
  for (auto const& object : lObjects)
    lMWID_Data[lID++] = object.second.filenamePosition;

  lCurrentPosition += 8 + lMWID_Size;

  // MDDF
  int lMDDF_Size = 0x24 * lModelInstances.size();
  lADTFile.Extend(8 + lMDDF_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MDDF', lMDDF_Size);
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mddf = lCurrentPosition - 0x14;

  // MDDF data
  auto const lMDDF_Data = lADTFile.GetPointer<ENTRY_MDDF>(lCurrentPosition + 8);

  if(world->mapIndex.sort_models_by_size_class())
  {
    std::sort(lModelInstances.begin(), lModelInstances.end(), [](ModelInstance const& m1, ModelInstance const& m2)
    {
      return m1.size_cat > m2.size_cat;
    });
  }

  lID = 0;
  for (auto const& model : lModelInstances)
  {
    auto filename_to_offset_and_name = lModels.find(model.model->filename);
    if (filename_to_offset_and_name == lModels.end())
    {
      LogError << "There is a problem with saving the doodads. We have a doodad that somehow changed the name during the saving function. However this got produced, you can get a reward from schlumpf by pasting him this line." << std::endl;
      return;
    }

    math::vector_3d const& pos = model.position();
    math::degrees::vec3 rotation = model.adt_rotation();

    lMDDF_Data[lID].nameID = filename_to_offset_and_name->second.nameID;
    lMDDF_Data[lID].uniqueID = model.uid;
    lMDDF_Data[lID].pos[0] = pos.x;
    lMDDF_Data[lID].pos[1] = pos.y;
    lMDDF_Data[lID].pos[2] = pos.z;
    lMDDF_Data[lID].rot[0] = rotation.x._;
    lMDDF_Data[lID].rot[1] = rotation.y._;
    lMDDF_Data[lID].rot[2] = rotation.z._;
    lMDDF_Data[lID].scale = (uint16_t)(model.scale() * 1024);
    lMDDF_Data[lID].flags = 0;
    lID++;
  }

  lCurrentPosition += 8 + lMDDF_Size;

  LogDebug << "Added " << lID << " doodads to MDDF" << std::endl;

  // MODF
  int lMODF_Size = 0x40 * lObjectInstances.size();
  lADTFile.Extend(8 + lMODF_Size);
  SetChunkHeader(lADTFile, lCurrentPosition, 'MODF', lMODF_Size);
  lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->modf = lCurrentPosition - 0x14;

  // MODF data
  auto const lMODF_Data = lADTFile.GetPointer<ENTRY_MODF>(lCurrentPosition + 8);

  lID = 0;
  for (auto const& object : lObjectInstances)
  {
    auto filename_to_offset_and_name = lObjects.find(object.wmo->filename);
    if (filename_to_offset_and_name == lObjects.end())
    {
      LogError << "There is a problem with saving the objects. We have an object that somehow changed the name during the saving function. However this got produced, you can get a reward from schlumpf by pasting him this line." << std::endl;
      return;
    }

    math::vector_3d const& pos = object.position();
    math::degrees::vec3 const& dir = object.adt_rotation();

    lMODF_Data[lID].nameID = filename_to_offset_and_name->second.nameID;
    lMODF_Data[lID].uniqueID = object.mUniqueID;
    lMODF_Data[lID].pos[0] = pos.x;
    lMODF_Data[lID].pos[1] = pos.y;
    lMODF_Data[lID].pos[2] = pos.z;
    lMODF_Data[lID].rot[0] = dir.x._;
    lMODF_Data[lID].rot[1] = dir.y._;
    lMODF_Data[lID].rot[2] = dir.z._;

    lMODF_Data[lID].extents[0][0] = object.extents[0].x;
    lMODF_Data[lID].extents[0][1] = object.extents[0].y;
    lMODF_Data[lID].extents[0][2] = object.extents[0].z;

    lMODF_Data[lID].extents[1][0] = object.extents[1].x;
    lMODF_Data[lID].extents[1][1] = object.extents[1].y;
    lMODF_Data[lID].extents[1][2] = object.extents[1].z;

    lMODF_Data[lID].flags = object.mFlags;
    lMODF_Data[lID].doodadSet = object.doodadset();
    lMODF_Data[lID].nameSet = object.mNameset;
    lMODF_Data[lID].unknown = object.mUnknown;
    lID++;
  }

  LogDebug << "Added " << lID << " wmos to MODF" << std::endl;

  lCurrentPosition += 8 + lMODF_Size;

  // MCNK
  for (int y = 0; y < 16; ++y)
  {
    for (int x = 0; x < 16; ++x)
    {
      mChunks[y][x]->save(lADTFile, lCurrentPosition, lMCIN_Position, lTextures, lObjectInstances, lModelInstances);
    }
  }

  // MFBO
  if (mFlags & 1)
  {
    size_t chunkSize = sizeof(int16_t) * 9 * 2;
    lADTFile.Extend(8 + chunkSize);
    SetChunkHeader(lADTFile, lCurrentPosition, 'MFBO', chunkSize);
    lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mfbo = lCurrentPosition - 0x14;

    auto const lMFBO_Data = lADTFile.GetPointer<int16_t>(lCurrentPosition + 8);

    lID = 0;

    for (int i = 0; i < 9; ++i)
      lMFBO_Data[lID++] = (int16_t)mMaximumValues[i].y;

    for (int i = 0; i < 9; ++i)
      lMFBO_Data[lID++] = (int16_t)mMinimumValues[i].y;

    lCurrentPosition += 8 + chunkSize;
  }

  //! \todo Do not do bullshit here in MTFX.
#if 0
  if (!mTextureEffects.empty()) {
    //! \todo check if nTexEffects == nTextures, correct order etc.
    lADTFile.Extend(8 + 4 * mTextureEffects.size());
    SetChunkHeader(lADTFile, lCurrentPosition, 'MTFX', 4 * mTextureEffects.size());
    lADTFile.GetPointer<MHDR>(lMHDR_Position + 8)->mtfx = lCurrentPosition - 0x14;

    auto const lMTFX_Data = lADTFile.GetPointer<uint32_t>(lCurrentPosition + 8);

    lID = 0;
    //they should be in the correct order...
    for (auto const& effect : mTextureEffects)
    {
      lMTFX_Data[lID] = effect;
      ++lID;
    }
    lCurrentPosition += 8 + sizeof(uint32_t) * mTextureEffects.size();
  }
#endif

  {
    MPQFile f(filename);
    // \todo This sounds wrong. There shouldn't *be* unused nulls to
    // begin with.
    f.setBuffer(lADTFile.data_up_to (lCurrentPosition)); // cleaning unused nulls at the end of file

    f.SaveFile();
  }

  lObjectInstances.clear();
  lModelInstances.clear();
  lModels.clear();
}


void MapTile::CropWater()
{
  for (int z = 0; z < 16; ++z)
  {
    for (int x = 0; x < 16; ++x)
    {
      Water.CropMiniChunk(x, z, mChunks[z][x].get());
    }
  }
}

void MapTile::set_shadows(std::vector<std::uint8_t> const& shadow_map, int threshold)
{
  for (int y = 0; y < 16; ++y)
  {
    for (int x = 0; x < 16; ++x)
    {
      mChunks[y][x]->set_shadows(shadow_map, threshold);
    }
  }
}

void MapTile::remove_model(uint32_t uid)
{
  std::lock_guard<std::mutex> const lock (_mutex);

  auto it = std::find(uids.begin(), uids.end(), uid);

  if (it != uids.end())
  {
    uids.erase(it);
  }
}

void MapTile::add_model(uint32_t uid)
{
  std::lock_guard<std::mutex> const lock(_mutex);

  if (std::find(uids.begin(), uids.end(), uid) == uids.end())
  {
    uids.push_back(uid);
  }
}

void MapTile::require_regular_alphamap()
{
  if (_use_no_alpha_alphamap)
  {
    _use_no_alpha_alphamap = false;
    _alphamap_created = false;
  }
}

void MapTile::create_combined_alpha_shadow_map()
{
  opengl::texture::set_active_texture(0);
  _adt_alphamap.bind();

  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);


  if (_use_no_alpha_alphamap)
  {
    std::vector<std::uint8_t> empty_amap_data(256 * 4, 0);
    gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB5_A1, 1, 1, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, empty_amap_data.data());
  }
  else
  {
    if (mBigAlpha)
    {
      gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 64, 64, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    else
    {
      gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB5_A1, 64, 64, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }


    for (size_t i = 0; i < 16; i++)
    {
      for (size_t j = 0; j < 16; j++)
      {
        mChunks[i][j]->update_alpha_shadow_map();
      }
    }
  }

  _alphamap_created = true;
}

void MapTile::upload()
{
  _vertex_array.upload();
  _vertex_buffers.upload();

  gl.bufferData<GL_ARRAY_BUFFER>(_vertices_vbo, sizeof(chunk_vertex) * mapbufsize * 256, NULL, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER>(_indices_vbo, sizeof(chunk_indice) * MapChunk::total_indices_count_with_lods() * 256, NULL, GL_STATIC_DRAW);

  // array of offsets and size for the glmultidraw
  for (int i = 0; i < 256; ++i)
  {
    _indices_offsets.push_back(static_cast<char*>(0) + i * MapChunk::total_indices_count_with_lods() * sizeof(chunk_indice));
    _indices_count.push_back(mChunks[i / 16][i % 16]->current_lod_indices_count());
  }

  _uploaded = true;
}

void MapTile::recalc_extents()
{
  _extents[0] = { xbase, std::numeric_limits<float>::max(), zbase };
  _extents[1] = { xbase + TILESIZE, std::numeric_limits<float>::min(), zbase + TILESIZE};

  for (int z = 0; z < 16; ++z)
  {
    for (int x = 0; x < 16; ++x)
    {
      _extents[0].y = std::min(_extents[0].y, mChunks[z][x]->vmin.y);
      _extents[1].y = std::max(_extents[1].y, mChunks[z][x]->vmax.y);
    }
  }

  if (Water.need_recalc_extents())
  {
    Water.need_recalc_extents();
  }

  _extents[0].y = std::min(_extents[0].y, Water.min_height());
  _extents[1].y = std::max(_extents[1].y, Water.max_height());


  _intersect_points.clear();
  _intersect_points = misc::intersection_points(_extents[0], _extents[1]);

  _need_recalc_extents = false;
  _need_visibility_update = true;
}

void MapTile::update_visibility ( const float& cull_distance
                                , const math::frustum& frustum
                                , const math::vector_3d& camera
                                , display_mode display
                                )
{
  static const float adt_radius = std::sqrt (TILESIZE * TILESIZE / 2.0f);

  float dist = display == display_mode::in_3D
             ? (camera - (_extents[0] + _extents[1]) * 0.5).length() - adt_radius // todo: improve when height diff > adt radius
             : std::abs(camera.y - _extents[1].y);


  bool old_value = _is_visible;

  _need_visibility_update = false;
  _is_visible = dist < cull_distance && frustum.intersects(_intersect_points);

  // todo: either sync the chunks' value with the adt's
  // or just use the value from the adt in the chunks
  // terrain rendering is so fast that culling individual chunks
  // slows down rendering because of the cpu bottleneck
  if (_is_visible && old_value != _is_visible)
  {
    for (size_t z = 0; z < 16; z++)
    {
      for (size_t x = 0; x < 16; x++)
      {
        mChunks[z][x]->set_visible();
        //mChunks[z][x]->update_visibility(cull_distance, frustum, camera, display);
      }
    }
  }
}
