// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <opengl/shader.fwd.hpp>
#include <opengl/types.hpp>

#include <math/vector_4d.hpp>
#include <math/vector_2d.hpp>

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

namespace math
{
  struct matrix_4x4;
  struct vector_2d;
  struct vector_3d;
  struct vector_4d;
}

namespace opengl
{
  // The caller guarantees that the array buffer is already bound.
  struct array_buffer_is_already_bound{};

  struct shader
  {
    shader(GLenum type, std::string const& source);
    ~shader();

    static std::string src_from_qrc(std::string const& shader_alias);
    static std::string src_from_qrc(std::string const& shader_alias, std::vector<std::string> const& defines);

    shader (shader const&) = delete;
    shader (shader&&) = delete;
    shader& operator= (shader const&) = delete;
    shader& operator= (shader&&) = delete;

  private:
    friend struct program;

    GLuint _handle;
  };

  struct program
  {
    program (std::initializer_list<shader>);
    ~program();

    program (program const&) = delete;
    program (program&&);
    program& operator= (program const&) = delete;
    program& operator= (program&&) = delete;

  private:
    inline GLuint uniform_location (std::string const& name) const;
    inline GLuint attrib_location (std::string const& name) const;

    inline GLuint uniform_block_index(std::string const& name) const;
    inline void uniform_block_binding(GLuint block_index, GLuint block_binding) const;

    friend struct scoped::use_program;

    std::optional<GLuint> _handle;
    mutable std::unordered_map<std::string, GLuint> _uniform_location_cache;
    mutable std::unordered_map<std::string, GLuint> _attrib_location_cache;
    mutable std::unordered_map<std::string, GLuint> _uniform_block_cache;
  };

  namespace scoped
  {
    class vao_binder;

    struct use_program
    {
      use_program (program const&);
      ~use_program();

      use_program (use_program const&) = delete;
      use_program (use_program&&) = delete;
      use_program& operator= (use_program const&) = delete;
      use_program& operator= (use_program&&) = delete;

      void uniform (std::string const& name, std::vector<int> const&);
      void uniform (std::string const& name, GLint);
      void uniform (std::string const& name, GLfloat);
      void uniform (std::string const& name, math::vector_2d const&);
      void uniform (std::string const& name, math::vector_2ui const&);
      void uniform (std::string const& name, math::vector_3d const&);
      void uniform (std::string const& name, math::vector_4d const&);
      void uniform (std::string const& name, math::vector_4i const&);
      void uniform (std::string const& name, math::matrix_4x4 const&);
      template<typename T> void uniform (std::string const&, T) = delete;

      void uniform_block_binding(std::string const& name, GLuint block_binding);

      void sampler (std::string const& name, GLenum texture_slot, texture*);

      // \note All attrib*() functions implicitly modify the state of the currently bound VAO.
      // Thus they ensure there is a VAO bound and the caller is aware of it being modified, by taking a reference to it.

      void attrib (vao_binder const&, std::string const& name, array_buffer_is_already_bound const&, math::matrix_4x4 const*, GLuint divisor = 0);
      void attrib (vao_binder const&, std::string const& name, GLuint buffer, math::matrix_4x4 const*, GLuint divisor = 0);
      void attrib (vao_binder const&, std::string const& name, array_buffer_is_already_bound const&, GLsizei size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid* data);
      void attrib (vao_binder const&, std::string const& name, GLuint buffer, GLsizei size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid* data);
      void attrib_int (vao_binder const&, std::string const& name, GLuint buffer, GLsizei size, GLenum type, GLsizei stride, const GLvoid* data);
      void attrib_int (vao_binder const&, std::string const& name, array_buffer_is_already_bound, GLsizei size, GLenum type, GLsizei stride, const GLvoid* data);

      void attrib_divisor(vao_binder const&, std::string const& name, GLuint divisor, GLsizei range = 1);

    private:
      GLuint uniform_location (std::string const& name);
      GLuint attrib_location (std::string const& name);

      std::unordered_map<std::string, GLuint> _uniforms;
      std::unordered_map<std::string, GLuint> _attribs;

      program const& _program;

      GLuint _old;
    };
  }
}
