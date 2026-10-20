// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TexturePicker.h>

#include <noggit/map_index.hpp>
#include <noggit/ui/font_awesome.hpp>
#include <noggit/ui/font_noggit.hpp>
#include <noggit/Selection.h>
#include <noggit/texture_set.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/World.h>
#include <noggit/tool_enums.hpp>

#include <QtWidgets/QGridLayout>
#include <QtWidgets/QPushButton>

namespace noggit
{
  namespace ui
  {
    texture_picker::texture_picker
        (current_texture* current_texture_window, QWidget* parent)
      : widget (parent)
      , layout (new ::QGridLayout(this))
      , _chunk (nullptr)
    {
      setWindowTitle ("Texture Picker");
      setWindowFlags (Qt::Tool | Qt::WindowStaysOnTopHint);

      for (int i = 0; i < 4; i++)
      {
        current_texture* click_label = new current_texture(false, this);
        connect ( click_label, &clickable_label::clicked
                , [=]
                  {
                    setTexture(i, current_texture_window);
                  }
                );

        layout->addWidget(click_label, 0, i);
        _labels.push_back(click_label);
      }

      QPushButton* btn_left = new QPushButton (this);
      QPushButton* btn_right = new QPushButton (this);
      btn_left->setIcon(font_awesome_icon(font_awesome::angledoubleleft));
      btn_right->setIcon(font_awesome_icon(font_awesome::angledoubleright));

      btn_left->setMinimumHeight(16);
      btn_right->setMinimumHeight(16);

      auto btn_layout(new QGridLayout);
      btn_layout->addWidget (btn_left, 0, 0);
      btn_layout->addWidget (btn_right, 0, 1);

      layout->addItem(btn_layout, 1, 0, 1, 4, Qt::AlignHCenter | Qt::AlignBottom);

      connect ( btn_left, &QPushButton::clicked
              , [this]
                {
                  emit shift_left();
                }
              );

      connect ( btn_right, &QPushButton::clicked
              , [this]
                {
                  emit shift_right();
                }
              );

      adjustSize();
      setFixedSize(size());

    }

    void texture_picker::getTextures(MapChunk* chunk)
    {
      show();

      if (chunk)
      {
        _chunk = chunk;
        _tile_index = chunk->mt->index;
        _chunk_x = chunk->px;
        _chunk_z = chunk->py;
        update(false);
      }
    }

    void texture_picker::invalidate()
    {
      _chunk = nullptr;
      _chunk_x = -1;
      _chunk_z = -1;
      _textures.clear();
      for (auto* label : _labels)
      {
        label->hide();
      }
    }

    bool texture_picker::revalidate(MapIndex const& map_index)
    {
      if (!_chunk)
      {
        return false;
      }
      MapTile* tile = map_index.tileLoaded(_tile_index)
        ? map_index.getTile(_tile_index)
        : nullptr;
      bool const alive = tile
        && _chunk_x >= 0 && _chunk_z >= 0
        && tile->getChunk(static_cast<unsigned>(_chunk_x), static_cast<unsigned>(_chunk_z)) == _chunk;
      if (!alive)
      {
        invalidate();
      }
      return alive;
    }

    void texture_picker::setTexture
      (size_t id, current_texture* current_texture_window)
    {
      if (!_chunk || id >= _textures.size())
      {
        return;
      }

      emit set_texture(_textures[id]);
      current_texture_window->set_texture (_textures[id]->filename);
    }

    void texture_picker::shiftSelectedTextureLeft()
    {
      if (!_chunk)
      {
        return;
      }
      auto&& selectedTexture = selected_texture::get();
      auto ts = _chunk->texture_set.get();
      for (int i = 1; i < ts->num(); i++)
      {
        if (ts->texture(i) == selectedTexture.value()->filename)
        {
          ts->swap_layers(i - 1, i);
          update();
          return;
        }
      }
    }

    void texture_picker::shiftSelectedTextureRight()
    {
      if (!_chunk)
      {
        return;
      }
      auto&& selectedTexture = selected_texture::get();
      auto ts = _chunk->texture_set.get();
      for (int i = 0; i < ts->num() - 1; i++)
      {
        if (ts->texture(i) == selectedTexture.value()->filename)
        {
          ts->swap_layers(i, i + 1);
          update();
          return;
        }
      }
    }

    void texture_picker::update(bool set_changed)
    {
      if (!_chunk)
      {
        _textures.clear();
        for (auto* label : _labels)
        {
          label->hide();
        }
        return;
      }

      if (set_changed)
      {
        _chunk->mt->changed = true;
      }

      _textures.clear();
      size_t index = 0;

      for (; index < _chunk->texture_set->num() && index < _labels.size(); ++index)
      {
        _textures.emplace_back(_chunk->texture_set->texture(index));
        _labels[index]->set_texture(_textures[index]->filename);
        _labels[index]->show();
      }

      for (; index < _labels.size(); ++index)
      {
        _labels[index]->hide();
      }
    }
  }
}
