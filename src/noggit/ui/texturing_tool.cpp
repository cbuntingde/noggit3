// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/texturing_tool.hpp>

#include <noggit/Misc.h>
#include <noggit/World.h>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/checkbox.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/slider_spinbox.hpp>
#include <noggit/ui/TexturePicker.h>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/TexturingGUI.h>

#include <util/qt/overload.hpp>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>

namespace noggit
{
  namespace ui
  {
    texturing_tool::texturing_tool ( const math::vector_3d* camera_pos
                                   , World* world
                                   , bool_toggle_property* show_quick_palette
                                   , QWidget* parent
                                   )
      : noggit_tool(parent)
      , _brush_level(255)
      , _radius(15.f)
      , _hardness(0.5f)
      , _pressure(0.9f)
      , _show_unpaintable_chunks(false)
      , _spray_size(1.0f)
      , _spray_pressure(2.0f)
      , _anim_prop(true)
      , _anim_speed_prop(1)
      , _anim_rotation_prop(4)
      , _overbright_prop(false)
      , _texturing_mode(texturing_mode::paint)
    {
      auto layout (new QFormLayout (this));

      _current_texture = new current_texture(true, this);
      _current_texture->resize(QSize(225, 225));
      layout->addRow (_current_texture);
      layout->setAlignment(_current_texture, Qt::AlignHCenter);

      _texture_picker = new ui::texture_picker(_current_texture);

      auto tabs (new QTabWidget(this));

      auto tool_widget (new QWidget (this));
      auto tool_layout (new QFormLayout (tool_widget));

      auto slider_layout (new QGridLayout);
      tool_layout->addRow(slider_layout);
      auto slider_layout_left (new QFormLayout(tool_widget));
      slider_layout->addLayout(slider_layout_left, 0, 0);
      auto slider_layout_right(new QVBoxLayout(tool_widget));
      slider_layout->addLayout(slider_layout_right, 0, 1);

      slider_layout_left->addRow(new slider_spinbox("Hardness", &_hardness, 0.f, 1.f, 2, this));
      slider_layout_left->addRow(new slider_spinbox("Radius", &_radius, 0.f, 100.f, 2, this));
      slider_layout_left->addRow(new slider_spinbox("Pressure", &_pressure, 0.f, 1.f, 2, this));


      _brush_level_slider = new QSlider (Qt::Orientation::Vertical, tool_widget);
      _brush_level_slider->setRange (0, 255);
      _brush_level_slider->setSliderPosition (_brush_level);

      QString _brush_level_slider_style =
        "QSlider::groove:vertical { \n "
        "  background-color: qlineargradient(x1:0.5, y1:0, x2:0.5, y2:1, stop: 0 black, stop: 1 #FFFFFF); \n "
        "  width: 35px; \n"
        "  margin: 0 0 0 0; \n "
        "} \n "
        "QSlider::handle:vertical { \n"
        "  background-color: red; \n"
        "  height: 5px; \n"
        "} \n"
        "QSlider::vertical { \n"
        "  width: 35px; \n"
        "} \n";

      _brush_level_slider->setStyleSheet(_brush_level_slider_style);
      slider_layout_right->addWidget(_brush_level_slider, 0, Qt::AlignHCenter);

      _brush_level_spin = new QSpinBox(tool_widget);
      _brush_level_spin->setRange(0, 255);
      _brush_level_spin->setValue(_brush_level);
      _brush_level_spin->setSingleStep(5);
      slider_layout_right->addWidget(_brush_level_spin);

      _show_unpaintable_chunks_cb = new QCheckBox("Show unpaintable chunks", tool_widget);
      _show_unpaintable_chunks_cb->setChecked(false);
      tool_layout->addRow(_show_unpaintable_chunks_cb);

      // spray
      _spray_mode_group = new QGroupBox("Spray", tool_widget);
      _spray_mode_group->setCheckable(true);
      tool_layout->addRow (_spray_mode_group);

      _spray_content = new QWidget(_spray_mode_group);
      auto spray_layout (new QFormLayout (_spray_content));
      _spray_mode_group->setLayout(spray_layout);

      _inner_radius_cb = new QCheckBox("Inner radius", _spray_content);
      spray_layout->addRow(_inner_radius_cb);

      spray_layout->addRow(new slider_spinbox("Size", &_spray_size, 1.f, 40.f, 2, _spray_content));
      spray_layout->addRow(new slider_spinbox("Pressure", &_spray_pressure, 0.f, 10.f, 2, _spray_content));

      _texture_switcher = new texture_swapper(tool_widget, camera_pos, world);
      _texture_switcher->hide();

      auto quick_palette_btn (new QPushButton("Quick Palette", this));
      tool_layout->addRow(quick_palette_btn);

      auto anim_widget (new QWidget (this));
      auto anim_layout (new QFormLayout (anim_widget));

      _anim_group = new QGroupBox("Add anim", anim_widget);
      _anim_group->setCheckable(true);
      _anim_group->setChecked(_anim_prop.get());

      auto anim_group_layout (new QFormLayout (_anim_group));

      auto anim_speed_slider = new QSlider(Qt::Orientation::Horizontal, _anim_group);
      anim_speed_slider->setRange(0, 7);
      anim_speed_slider->setSingleStep(1);
      anim_speed_slider->setTickInterval(1);
      anim_speed_slider->setTickPosition(QSlider::TickPosition::TicksBothSides);
      anim_speed_slider->setValue(_anim_speed_prop.get());
      anim_group_layout->addRow("Speed:", anim_speed_slider);

      anim_group_layout->addRow(new QLabel("Orientation:", _anim_group));

      auto anim_orientation_dial = new QDial(_anim_group);
      anim_orientation_dial->setRange(0, 8);
      anim_orientation_dial->setSingleStep(1);
      anim_orientation_dial->setValue(_anim_rotation_prop.get());
      anim_orientation_dial->setWrapping(true);
      anim_group_layout->addRow(anim_orientation_dial);

      anim_layout->addRow(_anim_group);

      auto overbright_cb = new checkbox("Overbright", &_overbright_prop, anim_widget);
      anim_layout->addRow(overbright_cb);

      tabs->addTab(tool_widget, "Paint");
      tabs->addTab(_texture_switcher, "Swap");
      tabs->addTab(anim_widget, "Anim");

      layout->addRow(tabs);

      connect ( _anim_group, &QGroupBox::toggled
              , [&](bool b)
                {
                  _anim_group->setTitle(QString(b ? "Add anim" : "Remove anim"));
                  _anim_prop.set(b);
                }
              );

      connect (anim_speed_slider, &QSlider::valueChanged, &_anim_speed_prop, &noggit::unsigned_int_property::set);
      connect (anim_orientation_dial, &QDial::valueChanged, &_anim_rotation_prop, &noggit::unsigned_int_property::set);

      connect ( tabs, &QTabWidget::currentChanged
              , [this] (int index)
                {
                  switch (index)
                  {
                    case 0: _texturing_mode = texturing_mode::paint; break;
                    case 1: _texturing_mode = texturing_mode::swap; break;
                    case 2: _texturing_mode = texturing_mode::anim; break;
                  }
                }
              );


      connect ( _brush_level_spin, qOverload<int> (&QSpinBox::valueChanged)
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_brush_level_slider);
                  _brush_level = v;
                  _brush_level_slider->setSliderPosition (v);
                }
              );

      connect ( _brush_level_slider, &QSlider::valueChanged
              , [&] (int v)
                {
                  QSignalBlocker const blocker (_brush_level_spin);
                  _brush_level = v;
                  _brush_level_spin->setValue(v);
                }
              );

      connect ( _show_unpaintable_chunks_cb, &QCheckBox::stateChanged
              , [&] (int state)
                {
                  _show_unpaintable_chunks = state;
                }
              );

      connect ( _spray_mode_group, &QGroupBox::toggled
              , [&] (bool b)
                {
                  _spray_content->setVisible(b);
                }
              );

      connect ( quick_palette_btn, &QPushButton::pressed
              , [=] ()
                {
                  show_quick_palette->set(true);
                }
              );

      connect( &_radius, qOverload<float>(&float_property::changed)
             , [&](float v)
               {
                 set_radius(v);
               }
             );
      connect( &_hardness, qOverload<float>(&float_property::changed)
             , [&](float)
               {
                 update_brush_hardness();
               }
             );
      connect( &_spray_size, qOverload<float>(&float_property::changed)
             , [&](float)
               {
                 update_spray_brush();
               }
             );

      _spray_content->hide();
      update_brush_hardness();
      update_spray_brush();
      set_radius(_radius.get());
      toggle_tool(); // to disable

      setMinimumWidth(sizeHint().width());
    }

    void texturing_tool::update_brush_hardness()
    {
      _texture_brush.set_inner_ratio(_hardness.get());
      _inner_brush.set_inner_ratio(_hardness.get());
      _spray_brush.set_inner_ratio(_hardness.get());
    }

    void texturing_tool::set_radius(float radius)
    {
      _texture_brush.set_radius(radius);
      _inner_brush.set_radius(radius * _hardness.get());
    }

    void texturing_tool::update_spray_brush()
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_brush.set_radius(_spray_size.get() * TEXDETAILSIZE / 2.0f);
      }
    }

    void texturing_tool::toggle_tool()
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_mode_group->setChecked(!_spray_mode_group->isChecked());
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->toggle_brush_mode();
      }
      else if (_texturing_mode == texturing_mode::anim)
      {
        _anim_group->setChecked(!_anim_group->isChecked());
      }
    }

    void texturing_tool::change_radius(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _radius.change(change);
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->change_radius(change);
      }
    }

    void texturing_tool::change_hardness(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _hardness.change(change);
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->change_hardness(change);
      }
    }

    void texturing_tool::change_pressure(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _pressure.change(change);
      }
      else if (_texturing_mode == texturing_mode::swap)
      {
        _texture_switcher->change_pressure(change);
      }
    }

    void texturing_tool::change_brush_level(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _brush_level_spin->setValue(std::ceil(_brush_level + change));
      }
    }

    void texturing_tool::set_brush_level (float level)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _brush_level_spin->setValue(level);
      }
    }

	void texturing_tool::toggle_brush_level_min_max()
	{
		if(_brush_level_spin->value() > _brush_level_spin->minimum())
			_brush_level_spin->setValue(_brush_level_spin->minimum());
		else _brush_level_spin->setValue(_brush_level_spin->maximum());
	}

    void texturing_tool::change_spray_size(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_size.change(change);
      }
    }

    void texturing_tool::change_spray_pressure(float change)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _spray_pressure.change(change);
      }
    }

    void texturing_tool::set_pressure(float pressure)
    {
      if (_texturing_mode == texturing_mode::paint)
      {
        _pressure.set(pressure);
      }
    }

    float texturing_tool::brush_radius() const
    {
      // show only a dot when using the anim / swap mode
      switch (_texturing_mode)
      {
        case texturing_mode::paint: return _radius.get();
        case texturing_mode::swap: return (_texture_switcher->brush_mode() ? _texture_switcher->radius() : 0.f);
        default: return 0.f;
      }
    }

    float texturing_tool::hardness() const
    {
      switch (_texturing_mode)
      {
        case texturing_mode::paint: return _hardness.get();
        case texturing_mode::swap: return _texture_switcher->brush_mode() ? _texture_switcher->hardness() : 0.f;
        default: return 0.f;
      }
    }

    bool texturing_tool::show_unpaintable_chunks() const
    {
      return _show_unpaintable_chunks && _texturing_mode == texturing_mode::paint;
    }

    void texturing_tool::paint (World* world, math::vector_3d const& pos, float dt, scoped_blp_texture_reference texture)
    {
      if (_texturing_mode == texturing_mode::swap)
      {
        float strength = 1.0f - pow(1.0f - _texture_switcher->pressure(), dt * 10.0f);

        auto to_swap (_texture_switcher->texture_to_swap());
        if (to_swap)
        {
          if (_texture_switcher->brush_mode())
          {
            world->replaceTexture(pos, _texture_switcher->brush(), strength, to_swap.value(), texture);
          }
          else
          {
            world->overwriteTextureAtCurrentChunk(pos, to_swap.value(), texture);
          }
        }
      }
      else if (_texturing_mode == texturing_mode::paint)
      {
        float strength = 1.0f - pow(1.0f - _pressure.get(), dt * 10.0f);

        if (_spray_mode_group->isChecked())
        {
          world->sprayTexture(pos, &_spray_brush, alpha_target(), strength, _radius.get(), _spray_pressure.get(), texture);

          if (_inner_radius_cb->isChecked())
          {
            world->paintTexture(pos, &_inner_brush, alpha_target(), strength, texture);
          }
        }
        else
        {
          world->paintTexture(pos, &_texture_brush, alpha_target(), strength, texture);
        }
      }
      else if (_texturing_mode == texturing_mode::anim)
      {
        change_tex_flag(world, pos, _anim_prop.get(), texture);
      }
    }


    void texturing_tool::revalidate_picker(World* world)
    {
      if (world && _texture_picker)
      {
        _texture_picker->revalidate(world->mapIndex);
      }
    }

    void texturing_tool::tick(float dt, math::vector_3d const& cursor_pos, bool cursor_under_map, World* world)
    {
      if (_left_mouse_button && !cursor_under_map)
      {
        if (_mod_shift_down && _mod_ctrl_down && _mod_alt_down)
        {
          // clear chunk textures
          world->eraseTextures(cursor_pos);
        }
        else if (_mod_ctrl_down)
        {
          // Pick texture
          _texture_picker->getTextures(world->get_chunk_at(cursor_pos));
        }
        else if (_mod_shift_down && !!noggit::ui::selected_texture::get())
        {
          paint(world, cursor_pos, dt, *noggit::ui::selected_texture::get());
        }
      }
    }

    void texturing_tool::mouse_move_event(QLineF const& relative_movement)
    {
      if (_right_mouse_button && _mod_alt_down)
      {
        change_hardness(relative_movement.dx() / (mouse_sensibility * 50.f));
      }
      else if (_left_mouse_button)
      {
        if (_mod_alt_down)
        {
          change_radius(relative_movement.dx() / mouse_sensibility);
        }
        else if (_mod_space_down)
        {
          change_pressure(relative_movement.dx() / (mouse_sensibility * 50.f));
        }
      }
    }

    void texturing_tool::wheel_event(QWheelEvent* event)
    {
      if (_mod_space_down)
      {
        change_brush_level(scroll_wheel_delta_for_range(event, 255.f));
      }
      else if (_mod_alt_down)
      {
        change_spray_size(scroll_wheel_delta_for_range(event, 39.f));
      }
      else if (_mod_shift_down)
      {
        change_spray_pressure(scroll_wheel_delta_for_range(event, 10.f));
      }
    }


    void texturing_tool::change_tex_flag(World* world, math::vector_3d const& pos, bool add, scoped_blp_texture_reference texture)
    {
      std::size_t flag = 0;

      flag |= FLAG_ANIMATE;

      // if add == true => flag to add, else it's the flags to remove
      if (add)
      {
        // the qdial in inverted compared to the anim rotation
        flag |= (_anim_rotation_prop.get() + 4) % 8;
        flag |= _anim_speed_prop.get() << 3;
      }
      else
      {
        flag |= 0xF;
      }

      // the texture's flag glow is set if the property is true, removed otherwise
      if (_overbright_prop.get())
      {
        flag |= FLAG_GLOW;
      }

      world->change_texture_flag(pos, texture, flag, add);
    }

    QSize texturing_tool::sizeHint() const
    {
      return QSize(215, height());
    }
  }
}
