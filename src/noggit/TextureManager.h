// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/AsyncObject.h>
#include <noggit/multimap_with_normalized_key.hpp>
#include <opengl/texture.hpp>


#include <map>
#include <optional>
#include <string>
#include <vector>

struct BLPHeader;

struct blp_texture : public opengl::texture, AsyncObject
{
  blp_texture (std::string const& filename);
  void finishLoading();

  void loadFromUncompressedData(BLPHeader const* lHeader, char const* lData, size_t file_size);
  void loadFromCompressedData(BLPHeader const* lHeader, char const* lData, size_t file_size);

  int width() const { return _width; }
  int height() const { return _height; }
  int layer_count() const { return _layer_count; }
  GLint texture_format() const;

#ifdef USE_BINDLESS_TEXTURES
  GLuint64 get_resident_handle();
#endif
  void bind();
  void upload();
  void upload_to_currently_bound_array(GLint array_layer, int starting_level = 0);

  virtual async_priority loading_priority() const
  {
    return async_priority::low;
  }

  int const public_id;

private:
  bool _uploaded = false;

  int _width;
  int _height;
  int _layer_count;

  std::optional<GLuint64> _handle;

private:
  std::map<int, std::vector<uint32_t>> _data;
  std::map<int, std::vector<uint8_t>> _compressed_data;
  std::optional<GLint> _compression_format;

  static std::atomic<int> blp_tex_counter;
};

struct scoped_blp_texture_reference;
class TextureManager
{
public:
  static void report();

private:
  friend struct scoped_blp_texture_reference;
  static noggit::async_object_multimap_with_normalized_key<blp_texture> _;
};

struct scoped_blp_texture_reference
{
  scoped_blp_texture_reference() = delete;
  scoped_blp_texture_reference (std::string const& filename);
  scoped_blp_texture_reference (scoped_blp_texture_reference const& other);
  scoped_blp_texture_reference (scoped_blp_texture_reference&&) = default;
  scoped_blp_texture_reference& operator= (scoped_blp_texture_reference const&) = delete;
  scoped_blp_texture_reference& operator= (scoped_blp_texture_reference&&) = default;
  ~scoped_blp_texture_reference() = default;

  blp_texture* operator->() const;
  blp_texture* get() const;

  int blp_id() const { return _blp_id; }

  bool operator== (scoped_blp_texture_reference const& other) const;

private:
  struct Deleter
  {
    void operator() (blp_texture*) const;
  };
  std::unique_ptr<blp_texture, Deleter> _blp_texture;
  int _blp_id = -1;
};

namespace noggit
{
  QPixmap render_blp_to_pixmap ( std::string const& blp_filename
                               , int width = -1
                               , int height = -1
                               );
}
