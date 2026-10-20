// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <opengl/context.hpp>
#include <opengl/texture.hpp>

#include <utility>

namespace opengl
{
  texture::texture()
    : _id (0)
  {

  }

  texture::~texture()
  {
    // Never call GL directly here: this destructor may run on any thread
    // (cache eviction, tile unload) with no GL context current, where
    // deleteTextures throws. The render loop runs the queued deletion.
    if (_id > 0 && _id != -1)
    {
      GLuint const id (_id);
      defer_gl_delete ([id]
        {
          GLuint tmp (id);
          gl.deleteTextures (1, &tmp);
        });
    }
  }

  texture::texture (texture&& other)
    : _id (other._id)
  {
    other._id = -1;
  }

  texture& texture::operator= (texture&& other)
  {
    std::swap (_id, other._id);
    return *this;
  }

  void texture::bind()
  {
    if (_id == 0)
    {
      gl.genTextures (1, &_id);
    }
    gl.bindTexture (GL_TEXTURE_2D, _id);
  }

  void texture::attach_to_framebuffer(GLenum attachment, int level)
  {
    if (_id == 0)
    {
      gl.genTextures(1, &_id);
    }

    gl.framebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, _id, level);
  }

  size_t texture::current_active_texture = -1;

  void texture::set_active_texture (size_t num)
  {
    if (num != current_active_texture)
    {
      gl.activeTexture(GL_TEXTURE0 + num);
      current_active_texture = num;
    }
  }

  void texture_array::bind()
  {
    if (_id == 0)
    {
      gl.genTextures(1, &_id);
    }
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, _id);
  }

#ifdef USE_BINDLESS_TEXTURES
  GLuint64 texture_array::get_resident_handle()
  {
    if (!_handle)
    {
      bind();
      _handle = gl.getTextureHandleARB(_id);
      gl.makeTextureHandleResidentARB(_handle.value());
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }

    return _handle.value();
  }
#endif
}
