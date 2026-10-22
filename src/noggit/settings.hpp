#pragma once

#include <QtCore/QSettings>

#include <memory>

namespace noggit
{
  class settings
  {
  public:
    settings()
    {
      values = std::make_unique<QSettings>("settings.ini", QSettings::Format::IniFormat);

      // Do not seed "project/path" with a default here. When the key is
      // missing, the application points it at the detected WoW installation,
      // which is what the script sandbox uses as its root. An empty value
      // means "not configured" and is treated as "no root" by the sandbox.
      QString project_folder = values->value("project/path", "").toString();
      if (!project_folder.isEmpty() && !(project_folder.endsWith('\\') || project_folder.endsWith('/')))
      {
        project_folder += "/";
      }

      if (!project_folder.isEmpty())
      {
        values->setValue("project/path", project_folder);
      }

      uids = std::make_unique<QSettings>(project_folder + "uid.ini", QSettings::Format::IniFormat);
    }

    QVariant value(const QString& key, const QVariant& default_value = QVariant()) const
    {
      return values->value(key, default_value);
    }

    void set_value(const QString& key, const QVariant& value)
    {
      values->setValue(key, value);
    }

    std::string project_path()
    {
      return values->value("project/path", "").toString().toStdString();
    }

    void sync()
    {
      values->sync();
    }

    std::unique_ptr<QSettings> values;
    std::unique_ptr<QSettings> uids;

    static settings& instance()
    {
      static settings inst;
      return inst;
    }
  };
}

#define NoggitSettings noggit::settings::instance()
