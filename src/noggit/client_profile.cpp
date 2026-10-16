// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/client_profile.hpp>

#include <noggit/Log.h>
#include <noggit/settings.hpp>

#include <QDir>

namespace
{
  char const* const vanilla_name = "vanilla_1_12_1";
  char const* const wotlk_name = "wotlk_3_3_5a";

  char const* const client_version_key = "project/client_version";
  char const* const game_path_key = "project/game_path";
}

namespace noggit
{
  namespace client_profile
  {
    QString to_string (client_version version)
    {
      switch (version)
      {
      case client_version::wotlk_3_3_5a:
        return wotlk_name;
      case client_version::vanilla_1_12_1:
      default:
        return vanilla_name;
      }
    }

    std::optional<client_version> from_string (QString const& name)
    {
      if (name == vanilla_name)
      {
        return client_version::vanilla_1_12_1;
      }
      if (name == wotlk_name)
      {
        return client_version::wotlk_3_3_5a;
      }
      return std::nullopt;
    }

    std::optional<client_version> detect_from_game_path (QString const& game_path)
    {
      if (game_path.isEmpty())
      {
        return std::nullopt;
      }

      QDir const data_folder (QDir (game_path).filePath ("Data"));

      // 1.x clients use a flat Data folder with base.MPQ and no locale
      // subfolders; common.MPQ and locale folders only exist in 2.x+
      if (data_folder.exists ("base.MPQ"))
      {
        return client_version::vanilla_1_12_1;
      }
      if (data_folder.exists ("common.MPQ"))
      {
        return client_version::wotlk_3_3_5a;
      }

      static char const* const locales[] = { "enGB", "enUS", "deDE", "koKR", "frFR"
                                           , "zhCN", "zhTW", "esES", "esMX", "ruRU" };
      for (auto const* locale : locales)
      {
        if (data_folder.exists (locale))
        {
          return client_version::wotlk_3_3_5a;
        }
      }

      return std::nullopt;
    }

    client_version current()
    {
      if (auto const stored = from_string (NoggitSettings.value (client_version_key).toString()))
      {
        // non-vanilla profiles are no longer supported; a stale setting
        // must not activate the dormant wotlk code paths
        if (*stored != client_version::vanilla_1_12_1)
        {
          return client_version::vanilla_1_12_1;
        }

        return *stored;
      }

      if (auto const detected = detect_from_game_path (NoggitSettings.value (game_path_key).toString()))
      {
        if (*detected != client_version::vanilla_1_12_1)
        {
          return client_version::vanilla_1_12_1;
        }

        return *detected;
      }

      return client_version::vanilla_1_12_1;
    }

    void set_current (client_version version)
    {
      if (version != client_version::vanilla_1_12_1)
      {
        LogError << "Client profile \"" << to_string (version).toStdString()
                 << "\" is no longer supported (WoW 1.12.1 only), using vanilla_1_12_1."
                 << std::endl;
        version = client_version::vanilla_1_12_1;
      }

      NoggitSettings.set_value (client_version_key, to_string (version));
    }

    std::vector<std::string> archive_names (client_version version)
    {
      if (version == client_version::vanilla_1_12_1)
      {
        // 1.12.1 flat data layout (as shipped by the 1.x clients)
        return { "base.MPQ"
               , "dbc.MPQ"
               , "fonts.MPQ"
               , "interface.MPQ"
               , "misc.MPQ"
               , "model.MPQ"
               , "sound.MPQ"
               , "speech.MPQ"
               , "terrain.MPQ"
               , "texture.MPQ"
               , "wmo.MPQ"
               , "backup.MPQ"
               , "patch.MPQ"
               , "patch-{number}.MPQ"
               , "development.MPQ"
               };
      }

      return { "common.MPQ"
             , "common-2.MPQ"
             , "expansion.MPQ"
             , "lichking.MPQ"
             , "patch.MPQ"
             , "patch-{number}.MPQ"
             , "patch-{character}.MPQ"
             , "{locale}/locale-{locale}.MPQ"
             , "{locale}/expansion-locale-{locale}.MPQ"
             , "{locale}/lichking-locale-{locale}.MPQ"
             , "{locale}/patch-{locale}.MPQ"
             , "{locale}/patch-{locale}-{number}.MPQ"
             , "{locale}/patch-{locale}-{character}.MPQ"
             , "development.MPQ"
             };
    }

    bool default_uid_startup_check (client_version version)
    {
      // the scan reads every existing tile of a map and blocks the editor
      // for tens of seconds; for vanilla the stored uid.ini value is
      // enough by default, the permission prompt covers first-time maps
      return version == client_version::wotlk_3_3_5a;
    }
  }
}
