// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace noggit
{
  namespace client_profile
  {
    // which client's file formats we read/write
    enum class client_version
    {
      vanilla_1_12_1,
      wotlk_3_3_5a,
    };

    QString to_string (client_version version);
    std::optional<client_version> from_string (QString const& name);

    // identifies the client from the game folder's data layout
    std::optional<client_version> detect_from_game_path (QString const& game_path);

    // stored setting, else detection from the configured game path,
    // else vanilla (the primary target)
    client_version current();
    void set_current (client_version version);

    // archives to load, relative to the client's Data folder;
    // wotlk archives can contain the "{locale}" placeholder
    std::vector<std::string> archive_names (client_version version);

    // whether entering a map runs the full 64x64 max UID scan by default;
    // the scan loads every tile of the map and takes tens of seconds
    bool default_uid_startup_check (client_version version);
  }
}
