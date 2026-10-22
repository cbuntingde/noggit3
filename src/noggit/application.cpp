// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/client_profile.hpp>
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MPQ.h>
#include <noggit/MapView.h>
#include <noggit/Model.h>
#include <noggit/ModelManager.h> // ModelManager::report()
#include <noggit/TextureManager.h> // TextureManager::report()
#include <noggit/WMO.h> // WMOManager::report()
#include <noggit/adt_roundtrip.hpp>
#include <noggit/errorHandling.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/settings.hpp>
#include <noggit/ui/main_window.hpp>
#include <opengl/context.hpp>
#include <util/exception_to_string.hpp>

#include <filesystem>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QtCore/QTimer>
#include <QtGui/QOffscreenSurface>
#include <QtOpenGL/QGLFormat>
#include <QtCore/QDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>

#include "revision.h"

namespace
{
  // thrown by the headless command modes (--adt-roundtrip, --asset-check,
  // --extract) to unwind out of Noggit's constructor so main can return the
  // status code; std::exit would skip destructors (Qt cleanup included)
  struct headless_exit
  {
    int code;
  };
}

class Noggit
{
public:
  Noggit (int argc, char *argv[]);

private:
  void initPath(char *argv[]);
  void loadMPQs();

  std::unique_ptr<noggit::ui::main_window> main_window;

  std::filesystem::path wowpath;

  bool fullscreen;
  bool doAntiAliasing;
};

void Noggit::initPath(char *argv[])
{
  try
  {
    std::filesystem::path startupPath(argv[0]);
    startupPath.remove_filename();

    if (startupPath.is_relative())
    {
      std::filesystem::current_path(std::filesystem::current_path() / startupPath);
    }
    else
    {
      std::filesystem::current_path(startupPath);
    }
  }
  catch (const std::filesystem::filesystem_error& ex)
  {
    LogError << ex.what() << std::endl;
  }
}

void Noggit::loadMPQs()
{
  // load project folder listfile first, todo: make that async
  {
    auto const prefix(std::filesystem::path(NoggitSettings.project_path()));
    auto const prefix_size(prefix.string().length());

    if (std::filesystem::exists(prefix))
    {
      for (auto const& entry_abs : std::filesystem::recursive_directory_iterator(prefix))
      {
        gListfile.emplace(
          noggit::mpq::normalized_filename
          (entry_abs.path().string().substr(prefix_size))
        );
      }
    }
  }

  auto const profile (noggit::client_profile::current());

  std::vector<std::string> archiveNames (noggit::client_profile::archive_names (profile));

  const char * locale("****");

  if (profile == noggit::client_profile::client_version::vanilla_1_12_1)
  {
    NOGGIT_LOG << "Using flat vanilla data layout (no locale subdirectories)." << std::endl;
  }
  else
  {
    const char * locales[] = { "enGB", "enUS", "deDE", "koKR", "frFR", "zhCN", "zhTW", "esES", "esMX", "ruRU" };

    // Find locale, take first one.
    for (int i(0); i < 10; ++i)
    {
      if (std::filesystem::exists (wowpath / "Data" / locales[i]))
      {
        locale = locales[i];
        NOGGIT_LOG << "Locale: " << locale << std::endl;
        break;
      }
    }
    if (!strcmp(locale, "****"))
    {
      LogError << "Could not find locale directory. Be sure, that there is one containing the file \"realmlist.wtf\"." << std::endl;
      //return -1;
    }
  }


  //! \todo  This may be done faster. Maybe.
  for (size_t i(0); i < archiveNames.size(); ++i)
  {
    std::string path((wowpath / "Data" / archiveNames[i]).string());

    path = misc::replace(path, "{locale}", std::string(locale));

    if (path.find("{number}") != std::string::npos)
    {
      for (char j = '2'; j <= '9'; j++)
      {
        std::string file = misc::replace(path, "{number}", std::string(1, j));

        if (std::filesystem::exists(file))
        {
          MPQArchive::loadMPQ(AsyncLoader::instance, file, true);
        }
      }
    }
    else if (path.find("{character}") != std::string::npos)
    {
      for (char c = 'a'; c <= 'z'; c++)
      {
        std::string file = misc::replace(path, "{character}", std::string(1, c));

        if (std::filesystem::exists(file))
        {
          MPQArchive::loadMPQ(AsyncLoader::instance, file, true);
        }
      }
    }
    else if (std::filesystem::exists(path))
    {
      MPQArchive::loadMPQ(AsyncLoader::instance, path, true);
    }
  }
}

namespace
{
  bool is_valid_game_path (const QDir& path)
  {
    if (!path.exists ())
    {
      LogError << "Path \"" << qPrintable (path.absolutePath ())
        << "\" does not exist." << std::endl;
      return false;
    }

    // 1.x clients use a flat Data folder (base.MPQ, terrain.MPQ, ...)
    // without locale subfolders
    if (path.exists ("Data/base.MPQ"))
    {
      return true;
    }

    QStringList locales;
    locales << "enGB" << "enUS" << "deDE" << "koKR" << "frFR"
      << "zhCN" << "zhTW" << "esES" << "esMX" << "ruRU";
    QString found_locale ("****");

    for (auto const& locale : locales)
    {
      if (path.exists (("Data/" + locale)))
      {
        found_locale = locale;
        break;
      }
    }

    if (found_locale == "****")
    {
      LogError << "Path \"" << qPrintable (path.absolutePath ())
        << "\" does not contain a locale directory "
        << "(invalid installation or no installation at all)."
        << std::endl;
      return false;
    }

    return true;
  }
}

namespace
{
  std::atomic_bool success = false;

  void opengl_context_creation_stuck_failsafe()
  {
    for (int i = 0; i < 50; ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (success.load())
      {
        return;
      }
    }

    LogError << "OpenGL Context creation failed (timeout), closing..." << std::endl;

    std::terminate();
  }
}

Noggit::Noggit(int argc, char *argv[])
  : fullscreen(false)
  , doAntiAliasing(true)
{
  InitLogging();
  assert (argc >= 1); (void) argc;
  initPath(argv);

  NOGGIT_LOG << "Noggit Studio - " << STRPRODUCTVER << std::endl;

#ifdef USE_BINDLESS_TEXTURES
  NOGGIT_LOG << "Bindless textures enabled." << std::endl;
#endif

  AsyncLoader::setup(NoggitSettings.value("async_thread_count", 3).toInt());

  doAntiAliasing = NoggitSettings.value("antialiasing", false).toBool();
  fullscreen = NoggitSettings.value("fullscreen", false).toBool();

  srand(::time(nullptr));
  QDir path (NoggitSettings.value ("project/game_path").toString());

  while (!is_valid_game_path (path))
  {
    auto const new_path (QFileDialog::getExistingDirectory (nullptr, "Open WoW Directory", "/", QFileDialog::ShowDirsOnly));
    if (new_path.isEmpty())
    {
      LogError << "Could not auto-detect game path "
        << "and user canceled the dialog." << std::endl;
      throw std::runtime_error ("Could not auto-detect game path and user canceled the dialog.");
    }
    path = QDir (new_path);
  }

  // save the new game path if it was changed
  NoggitSettings.set_value("project/game_path", path.absolutePath());
  NoggitSettings.sync();

  wowpath = path.absolutePath().toStdString();

  NOGGIT_LOG << "Game path: " << wowpath << std::endl;

  // The project path defaults to the WoW installation. A legacy "./" value
  // means it was never configured, so treat it as unset as well: otherwise
  // the script sandbox would use the install directory as its root.
  QString project_path = NoggitSettings.value ("project/path").toString();
  if (project_path.isEmpty() || project_path == "./")
  {
    project_path = path.absolutePath();
  }
  if (!project_path.endsWith('/') && !project_path.endsWith('\\'))
  {
    project_path += "/";
  }
  NoggitSettings.set_value ("project/path", project_path);

  NOGGIT_LOG << "Project path: " << project_path.toStdString() << std::endl;

  NoggitSettings.set_value ("project/game_path", path.absolutePath());

  // resolve the client profile; detection from the game folder's layout
  // wins, else the stored value, else vanilla (the only supported target)
  if (auto const detected = noggit::client_profile::detect_from_game_path (path.absolutePath()))
  {
    noggit::client_profile::set_current (*detected);
    NOGGIT_LOG << "Client profile: "
      << noggit::client_profile::to_string (noggit::client_profile::current()).toStdString()
      << " (detected from game path)" << std::endl;
  }
  else
  {
    NOGGIT_LOG << "Client profile: "
      << noggit::client_profile::to_string (noggit::client_profile::current()).toStdString()
      << " (game path layout unknown, using stored/default profile)" << std::endl;
  }

  loadMPQs(); // listfiles are not available straight away! They are async! Do not rely on anything at this point!
  OpenDBs();

  // headless ADT round-trip check; runs before any window or GL context
  // is created so it stays usable on machines without display support
  int roundtrip_map_id = -1;
  int roundtrip_tile_x = -1;
  int roundtrip_tile_z = -1;
  std::vector<std::pair<std::string, std::string>> extract_files;
  std::vector<std::string> asset_check_paths;

  for (int i = 1; i < argc; ++i)
  {
    if (!std::strcmp(argv[i], "--adt-roundtrip") && i + 1 < argc)
    {
      roundtrip_map_id = std::atoi(argv[++i]);
    }
    else if (!std::strcmp(argv[i], "--adt-roundtrip-tile") && i + 1 < argc)
    {
      std::sscanf(argv[++i], "%d,%d", &roundtrip_tile_x, &roundtrip_tile_z);
    }
    else if (!std::strcmp(argv[i], "--extract") && i + 2 < argc)
    {
      std::string const archive_path (argv[++i]);
      extract_files.emplace_back (archive_path, argv[++i]);
    }
    else if (!std::strcmp(argv[i], "--asset-check") && i + 1 < argc)
    {
      asset_check_paths.emplace_back (argv[++i]);
    }
  }

  // headless asset load check: construct and fully load M2/WMO files through
  // the real loaders without creating a GL context, so the vanilla parsing can
  // be validated on real client data
  if (!asset_check_paths.empty())
  {
    int failures = 0;
    int loaded = 0;

    std::vector<std::unique_ptr<Model>> models;
    std::vector<std::unique_ptr<WMO>> wmos;

    for (auto const& path : asset_check_paths)
    {
      if (!MPQFile::exists (path))
      {
        LogError << "asset-check: \"" << path << "\" not found in the archives." << std::endl;
        ++failures;
        continue;
      }

      bool const is_wmo (path.size() >= 4
                      && (path.compare (path.size() - 4, 4, ".wmo") == 0
                       || path.compare (path.size() - 4, 4, ".WMO") == 0));

      // the numbered group archives (..._000.wmo) are not standalone WMOS
      if (is_wmo && path.size() >= 8 && path[path.size() - 8] == '_'
        && std::isdigit (static_cast<unsigned char> (path[path.size() - 7])))
      {
        NOGGIT_LOG << "asset-check: skipping WMO group file " << path << std::endl;
        continue;
      }

      if (is_wmo)
      {
        auto wmo = std::make_unique<WMO> (path);
        AsyncLoader::instance->queue_for_load (wmo.get());
        wmos.push_back (std::move (wmo));
      }
      else
      {
        auto model = std::make_unique<Model> (path);
        AsyncLoader::instance->queue_for_load (model.get());
        models.push_back (std::move (model));
      }
    }

    AsyncLoader::instance->wait_queue_empty();

    for (auto const& model : models)
    {
      if (model->loading_failed())
      {
        LogError << "asset-check: FAILED M2 " << model->filename << std::endl;
        ++failures;
      }
      else
      {
        NOGGIT_LOG << "asset-check: ok M2 " << model->filename
          << " vanilla=" << model->is_vanilla()
          << " bones=" << model->bone_count()
          << " particles=" << model->particle_system_count()
          << " ribbons=" << model->ribbon_count()
          << " lights=" << model->light_count()
          << std::endl;
        ++loaded;
      }
    }

    for (auto const& wmo : wmos)
    {
      if (wmo->loading_failed())
      {
        LogError << "asset-check: FAILED WMO " << wmo->filename << std::endl;
        ++failures;
      }
      else
      {
        int liquid_groups = 0;
        int liquid_tiles = 0;
        int liquid_vertices = 0;

        for (auto const& group : wmo->groups)
        {
          if (!group.liquid)
          {
            continue;
          }

          ++liquid_groups;
          liquid_tiles += group.liquid->tile_count();
          liquid_vertices += group.liquid->vertex_count();
        }

        NOGGIT_LOG << "asset-check: ok WMO " << wmo->filename
          << " groups=" << wmo->groups.size()
          << " materials=" << wmo->materials.size()
          << " textures=" << wmo->textures.size()
          << " liquid_groups=" << liquid_groups
          << " liquid_tiles=" << liquid_tiles
          << " liquid_vertices=" << liquid_vertices
          << " has_liquids=" << wmo->has_liquids()
          << std::endl;
        ++loaded;
      }
    }

    NOGGIT_LOG << "asset-check: " << loaded << " ok, " << failures << " failed." << std::endl;

    throw headless_exit{failures ? 1 : 0};
  }

  // dump files from the client archives to disk; used to get ground truth
  // on vanilla file layouts the loaders have to parse
  if (!extract_files.empty())
  {
    int failures = 0;

    for (auto const& [archive_path, output_path] : extract_files)
    {
      // MPQFile's constructor throws for a missing file, so check first
      if (!MPQFile::exists (archive_path))
      {
        LogError << "extract: file \"" << archive_path << "\" not found in the archives." << std::endl;
        ++failures;
        continue;
      }

      MPQFile extracted (archive_path);
      extracted.save_file (output_path);
    }

    throw headless_exit{failures ? 1 : 0};
  }

  if (roundtrip_map_id >= 0)
  {
    std::string map_name;

    for (DBCFile::Iterator it = gMapDB.begin(); it != gMapDB.end(); ++it)
    {
      if (it->getInt(MapDB::MapID) == roundtrip_map_id)
      {
        map_name = it->getString(MapDB::InternalName);
        break;
      }
    }

    if (map_name.empty())
    {
      LogError << "adt_roundtrip: map id " << roundtrip_map_id << " does not exist in Map.dbc." << std::endl;
      throw headless_exit{1};
    }

    throw headless_exit{noggit::run_adt_roundtrip (map_name, roundtrip_map_id, roundtrip_tile_x, roundtrip_tile_z)};
  }

  if (!QGLFormat::hasOpenGL())
  {
    throw std::runtime_error ("Your system does not support OpenGL. Sorry, this application can't run without it.");
  }

  QSurfaceFormat format;

  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);

  format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);

  bool vsync = NoggitSettings.value("vsync", true).toBool();

  format.setSwapInterval(vsync ? 1 : 0);

  if (doAntiAliasing)
  {
    format.setSamples (4);
  }

  // context creation seems to get stuck sometimes, this ensure the app is killed
  // otherwise it's wasting cpu resources and is annoying when developping
  auto failsafe = std::async(&opengl_context_creation_stuck_failsafe);

  QSurfaceFormat::setDefaultFormat (format);

  QOpenGLContext context;
  context.create();
  QOffscreenSurface surface;
  surface.create();
  context.makeCurrent (&surface);

  success = true;

  opengl::context::scoped_setter const _ (::gl, &context);

  LogDebug << "GL: Version: " << gl.getString (GL_VERSION) << std::endl;
  LogDebug << "GL: Vendor: " << gl.getString (GL_VENDOR) << std::endl;
  LogDebug << "GL: Renderer: " << gl.getString (GL_RENDERER) << std::endl;

  // ensure all MPQs are loaded fully so the listfile is complete
  AsyncLoader::instance->wait_queue_empty();

  main_window = std::make_unique<noggit::ui::main_window>();
  if (fullscreen)
  {
    main_window->showFullScreen();
  }
  else
  {
    main_window->showMaximized();
  }
}

namespace
{
  void noggit_terminate_handler()
  {
    std::string const reason
      {util::exception_to_string (std::current_exception())};

    if (qApp)
    {
      QMessageBox::critical ( nullptr
                            , "std::terminate"
                            , QString::fromStdString (reason)
                            , QMessageBox::Close
                            , QMessageBox::Close
                            );
    }

    LogError << "std::terminate: " << reason << std::endl;
  }

  struct application_with_exception_printer_on_notify : QApplication
  {
    using QApplication::QApplication;

    virtual bool notify (QObject* object, QEvent* event) override
    {
      try
      {
        return QApplication::notify (object, event);
      }
      catch (...)
      {
        std::terminate();
      }
    }
  };
}

int main(int argc, char *argv[])
{
  noggit::RegisterErrorHandlers();
  std::set_terminate (noggit_terminate_handler);

  QApplication qapp (argc, argv);
  qapp.setApplicationName ("Noggit");
  qapp.setOrganizationName ("Noggit");

  int exit_code = 0;
  try
  {
    Noggit app (argc, argv);

    exit_code = qapp.exec();
  }
  catch (headless_exit const& headless)
  {
    exit_code = headless.code;
  }

  // stop the loader threads now that the application is torn down, so that no
  // thread is still working on objects that are about to be destroyed
  AsyncLoader::shutdown();

  return exit_code;
}
