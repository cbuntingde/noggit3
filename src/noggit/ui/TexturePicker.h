// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/MapChunk.h>
#include <noggit/Selection.h>
#include <noggit/tile_index.hpp>
#include <noggit/ui/widget.hpp>

#include <vector>

class MapIndex;
class QGridLayout;

namespace noggit
{
  namespace ui
  {
    class current_texture;

    class texture_picker : public widget
    {
      Q_OBJECT

    public:
      texture_picker (current_texture*, QWidget* parent = nullptr);

      void getTextures(MapChunk* chunk);
      void setTexture(size_t id, current_texture*);
      void shiftSelectedTextureLeft();
      void shiftSelectedTextureRight();

      //! Drop the cached chunk without touching it. The chunk may already
      //! be destroyed (tile unload); never dereference _chunk to clear it.
      void invalidate();
      //! Re-check the cached chunk against the loaded tiles. Unloads and
      //! reloads replace MapChunk objects, so pointer identity is verified,
      //! not just tile presence. Returns whether _chunk is still usable.
      bool revalidate(MapIndex const& map_index);

    private:
      void update(bool set_changed = true);

      QGridLayout* layout;

      std::vector<current_texture*> _labels;
      std::vector<scoped_blp_texture_reference> _textures;
      MapChunk* _chunk;
      //! Identity of the cached chunk's tile, stamped at pick time so the
      //! chunk can be revalidated after unloads/reloads without touching it.
      tile_index _tile_index{0, 0};
      int _chunk_x = -1;
      int _chunk_z = -1;

    signals:
      void set_texture(scoped_blp_texture_reference texture);
      void shift_left();
      void shift_right();
    };
  }
}
