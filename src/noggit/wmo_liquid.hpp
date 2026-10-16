// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// #define USEBLSFILES

#include <math/vector_2d.hpp>
#include <math/vector_3d.hpp>
#include <noggit/MPQ.h>
#include <noggit/TextureManager.h>
#include <noggit/liquid_render.hpp>
#include <noggit/wmo_headers.hpp>
#include <opengl/scoped.hpp>

#include <memory>



class wmo_liquid
{
public:
  wmo_liquid(MPQFile* f, WMOLiquidHeader const& header, WMOMaterial const& mat, int group_liquid, bool use_dbc_type, bool is_ocean);
  wmo_liquid(wmo_liquid const& other);

  void draw ( math::matrix_4x4 const& transform
            , liquid_render& render
            );

  void upload(opengl::scoped::use_program& water_shader, liquid_render& render);

  int tile_count() const { return xtiles * ytiles; }
  int vertex_count() const { return static_cast<int>(_vertices.size()); }

private:
  int initGeometry(MPQFile* f);

  math::vector_3d pos;
  bool mTransparency;
  int xtiles, ytiles;
  int _liquid_id;

  std::vector<liquid_vertex> _vertices;

  std::vector<std::uint16_t> indices;

  int _indices_count;

  bool _uploaded = false;

  opengl::scoped::deferred_upload_buffers<3> _buffer;
  GLuint const& _indices_buffer = _buffer[0];
  GLuint const& _vertices_buffer = _buffer[1];
  GLuint const& _liquid_ubo = _buffer[2];

  opengl::scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
};
