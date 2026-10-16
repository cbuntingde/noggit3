// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <qt-color-widgets/color_selector.hpp>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QWidget>

namespace util
{
  struct file_line_edit : public QWidget
  {
    enum mode
    {
      files,
      directories,
    };

    file_line_edit (mode, QString browse_title, QWidget* parent);

    QLineEdit* actual;
  };
}

namespace noggit
{
  namespace ui
  {
    class settings : public QDialog
    {
    private:
      util::file_line_edit* gamePathField;
      util::file_line_edit* projectPathField;
      util::file_line_edit* importPathField;
      util::file_line_edit* wmvLogPathField;
      util::file_line_edit* mclq_liquids_export_path;
      QComboBox* _client_version_combo;
      QDoubleSpinBox* _fov;
      QDoubleSpinBox* _view_distance;
      QSpinBox* _adt_unload_dist;
      QSpinBox* _adt_unload_check_interval;
      QSpinBox* _adt_loading_radius;
      QSpinBox* _async_loader_thread_count;
      QCheckBox* _uid_cb;

      QCheckBox* tabletModeCheck;
      QCheckBox* _undock_tool_properties;
      QCheckBox* _undock_small_texture_palette;
      QCheckBox* _vsync_cb;

      QCheckBox* _additional_file_loading_log;

      QCheckBox* _use_mclq_liquids_export;
      QCheckBox* _only_one_model_box;

#ifdef NOGGIT_HAS_SCRIPTING
      QCheckBox* _allow_scripts_write_any_file;
#endif

      QGroupBox* _mysql_box;
#ifdef USE_MYSQL_UID_STORAGE
      QLineEdit* _mysql_server_field;
      QLineEdit* _mysql_user_field;
      QLineEdit* _mysql_pwd_field;
      QLineEdit* _mysql_db_field;
#endif

      QButtonGroup* _wireframe_type_group;
      QDoubleSpinBox* _wireframe_radius;
      QDoubleSpinBox* _wireframe_width;
      color_widgets::ColorSelector* _wireframe_color;
      QCheckBox* _anti_aliasing_cb;
      QCheckBox* _fullscreen_cb;

    public:
      settings(QWidget* parent = nullptr);
      void discard_changes();
      void save_changes();
    };
  }
}
