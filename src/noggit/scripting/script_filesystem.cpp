// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/scripting/script_exception.hpp>
#include <noggit/scripting/script_filesystem.hpp>
#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_context.hpp>
#include <noggit/settings.hpp>
#include <noggit/ui/SettingsPanel.h>

#include <QtWidgets/QMessageBox>
#include <sol/sol.hpp>

#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace noggit
{
  namespace scripting
  {
    void mkdirs(std::string const& pathstr)
    {
      auto path = fs::path(pathstr);
      auto parent_path = path.parent_path();
      if (parent_path.string().size() > 0)
      {
        fs::create_directories(path.parent_path());
      }
    }

    namespace
    {
      bool path_is_within (fs::path const& candidate, fs::path const& root)
      {
        auto candidate_it (candidate.begin());
        auto root_it (root.begin());
        for (; root_it != root.end(); ++root_it, ++candidate_it)
        {
          if (candidate_it == candidate.end() || *candidate_it != *root_it)
          {
            return false;
          }
        }
        return true;
      }

      fs::path canonicalize (std::string const& path)
      {
        std::error_code ec;
        // weakly_canonical resolves ".." and symlinks where possible without
        // requiring the path to exist; absolute anchors relative paths at cwd
        return fs::weakly_canonical (fs::absolute (fs::path (path)), ec);
      }

      //! A root that resolves to the filesystem root would make every path on
      //! the system readable, so such a root must not be used as a sandbox root.
      bool is_usable_root (fs::path const& root)
      {
        if (root.empty() || root == root.root_path())
        {
          return false;
        }
        return true;
      }

      //! Scripts may only read inside the scripts tree (including the
      //! directory of the running script) and the open WoW project.
      //! Anything else (home dir, credentials, other projects) is denied.
      bool is_readable_path (script_context* state, fs::path const& canonical)
      {
        try
        {
          fs::path const scripts_root (canonicalize ("scripts"));
          fs::path const script_dir_root (canonicalize (state->current_script_dir()));
          std::string const project_path (NoggitSettings.project_path());

          if (is_usable_root(scripts_root) && path_is_within (canonical, scripts_root))
            return true;
          if (is_usable_root(script_dir_root) && path_is_within (canonical, script_dir_root))
            return true;
          if (!project_path.empty())
          {
            fs::path const project_root (canonicalize (project_path));
            if (is_usable_root(project_root) && path_is_within (canonical, project_root))
              return true;
          }
        }
        catch (...)
        {
        }
        return false;
      }

    }

    fs::path get_readable_path (std::string const& caller, script_context* state, std::string const& path)
    {
      std::error_code ec;
      fs::path const canonical (fs::weakly_canonical (fs::absolute (fs::path (path)), ec));
      if (ec || !is_readable_path (state, canonical))
      {
        throw script_exception (caller, "reading outside the scripts and project directories is not allowed: " + path);
      }
      return canonical;
    }

    std::string read_file(script_context* ctx, std::string const& path)
    {
      auto readable (get_readable_path ("read_file", ctx, path));
      if (!fs::exists(readable))
      {
        throw script_exception("read_file","no such file:" + path);
      }
      std::ifstream t(readable);
      std::string str((std::istreambuf_iterator<char>(t)),
              std::istreambuf_iterator<char>());
      return str;
    }

    std::filesystem::path get_writable_path(std::string const& caller, script_context * state, std::string const& path)
    {
      // anchor relative paths at the current directory, so that the prompt
      // shows the user where the file will actually end up
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::path(path)), ec);
      if (ec)
      {
        throw script_exception(caller,"cannot resolve the path "+path);
      }
      if (state->tool()->get_noggit_settings()->value("allow_scripts_write_any_file", false).toBool())
      {
        return canonical;
      }
      // the consent is per script context, so that approving a path for one
      // script does not silently allow every other script to write it
      if (state->allowed_write_files.find(canonical) != state->allowed_write_files.end())
      {
        return canonical;
      }
      QMessageBox prompt;
      prompt.setText(std::string("A script wants to write to the file "+canonical.string()).c_str());
      prompt.setInformativeText(std::string("Do you want to allow the script to write to "+canonical.string()+"?").c_str());
      prompt.setStandardButtons(QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
      prompt.setDefaultButton(QMessageBox::No);
      bool answer = prompt.exec() == QMessageBox::StandardButton::Yes;
      if(!answer)
      {
        throw script_exception(caller,"No permission to write file "+canonical.string());
      }
      return *state->allowed_write_files.emplace (canonical).first;
    }

    void write_file(script_context * ctx, std::string const& path, std::string const& input)
    {
      auto writable_path = get_writable_path("write_file",ctx,path);
      mkdirs(writable_path.string());
      std::ofstream(writable_path.string()) << input;
    }

    void append_file(script_context * ctx, std::string const& path, std::string const& input)
    {
      auto writable_path = get_writable_path("append_file",ctx,path);
      mkdirs(writable_path.string());
      std::ofstream outfile;
      outfile.open(writable_path.string(), std::ios_base::app); // append instead of overwrite
      outfile << input;
    }

    bool path_exists(script_context* ctx, std::string const& path)
    {
      // Outside the sandbox the file is simply not visible; this also
      // closes the existence oracle without raising script errors.
      std::error_code ec;
      fs::path const canonical (fs::weakly_canonical (fs::absolute (fs::path (path)), ec));
      if (ec || !is_readable_path (ctx, canonical))
      {
        return false;
      }
      return fs::exists(canonical, ec) && !ec;
    }

    bool is_path_within (std::filesystem::path const& path, std::filesystem::path const& root)
    {
      std::error_code ec;
      fs::path const canonical_path (fs::weakly_canonical (fs::absolute (path), ec));
      fs::path const canonical_root (fs::weakly_canonical (fs::absolute (root), ec));
      if (ec)
      {
        return false;
      }
      return path_is_within (canonical_path, canonical_root);
    }

    void register_filesystem(script_context * state)
    {
      state->set_function("write_file", [state](
          std::string const& path
        , std::string const& input
        ) {
          write_file(state, path, input);
        });
      state->set_function("append_file", [state](
          std::string const& path
        , std::string const& input
        ) {
          append_file(state, path, input);
        });
      state->set_function("read_file", [state](std::string const& path) {
        return read_file(state, path);
      });
      state->set_function("path_exists", [state](std::string const& path) {
        return path_exists(state, path);
      });
    }
  } // namespace scripting
} // namespace noggit
