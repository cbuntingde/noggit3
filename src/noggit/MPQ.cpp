// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h> // AsyncLoader
#include <noggit/Log.h>
#include <noggit/MPQ.h>
#include <noggit/settings.hpp>

#include <limits>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>

namespace
{
  typedef std::pair<std::string, std::unique_ptr<MPQArchive>> ArchiveEntry;
  typedef std::list<ArchiveEntry> ArchivesMap;
  ArchivesMap _openArchives;

  std::mutex gListfileLoadingMutex;
  std::mutex gMPQFileMutex;
}

std::unordered_set<std::string> gListfile;

void MPQArchive::loadMPQ (AsyncLoader* loader, std::string const& filename, bool doListfile)
{
  _openArchives.emplace_back (filename, std::make_unique<MPQArchive> (filename, doListfile));
  loader->queue_for_load(_openArchives.back().second.get());
}

MPQArchive::MPQArchive(std::string const& filename_, bool doListfile)
  : AsyncObject(filename_)
  ,_archiveHandle(nullptr)
{
  if (!SFileOpenArchive (filename.c_str(), 0, MPQ_OPEN_NO_LISTFILE | STREAM_FLAG_READ_ONLY, &_archiveHandle))
  {
    LogError << "Error opening archive: " << filename << std::endl;
    return;
  }
  else
  {
    LogDebug << "Opened archive " << filename << std::endl;
  }

  finished = !doListfile;
}

void MPQArchive::finishLoading()
{
  if (finished)
    return;

  HANDLE fh;

  std::lock_guard<std::mutex> lock2(gMPQFileMutex);
  std::lock_guard<std::mutex> lock(gListfileLoadingMutex);

  if (SFileOpenFileEx(_archiveHandle, "(listfile)", 0, &fh))
  {
    size_t filesize = SFileGetFileSize(fh, nullptr); //last nullptr for newer version of StormLib

    std::vector<char> readbuffer (filesize);
    SFileReadFile(fh, readbuffer.data(), filesize, nullptr, nullptr); //last nullptrs for newer version of StormLib
    SFileCloseFile(fh);

    std::string current;
    for (char c : readbuffer)
    {
      if (c == '\r')
      {
        continue;
      }
      if (c == '\n')
      {
        gListfile.emplace (noggit::mpq::normalized_filename (current));
        current.resize (0);
      }
      else
      {
        current += c;
      }
    }

    if (!current.empty())
    {
      gListfile.emplace (noggit::mpq::normalized_filename (current));
    }
  }

  finished = true;
  _state_changed.notify_all();

  if (MPQArchive::allFinishedLoading())
  {
    LogDebug << "Completed listfile loading: " << gListfile.size() << " files\n";
  }
}

MPQArchive::~MPQArchive()
{
  if (_archiveHandle)
    SFileCloseArchive(_archiveHandle);
}

bool MPQArchive::allFinishedLoading()
{
  return std::all_of ( _openArchives.begin(), _openArchives.end()
                     , [] (ArchiveEntry const& archive)
                       {
                         return archive.second->finishedLoading();
                       }
                     );
}

void MPQArchive::allFinishLoading()
{
  for (auto& archive : _openArchives)
  {
    archive.second->finishLoading();
  }
}

void MPQArchive::unloadAllMPQs()
{
  _openArchives.clear();
}

bool MPQArchive::hasFile(std::string const& file) const
{
  return SFileHasFile(_archiveHandle, noggit::mpq::normalized_filename_insane (file).c_str());
}

void MPQArchive::unloadMPQ(std::string const& filename)
{
  for (auto it = _openArchives.begin(); it != _openArchives.end(); ++it)
  {
    if (it->first == filename)
    {
      _openArchives.erase(it);
      break;
    }
  }
}

bool MPQArchive::openFile(std::string const& file, HANDLE* fileHandle) const
{
  assert(fileHandle);
  return SFileOpenFileEx(_archiveHandle, noggit::mpq::normalized_filename_insane (file).c_str(), 0, fileHandle);
}

namespace
{
  std::filesystem::path getDiskPath (std::string const& normalized_filename)
  {
    return std::filesystem::path (NoggitSettings.project_path())
      / normalized_filename;
  }

  bool existsInMPQ (std::string const& filename)
  {
    return std::any_of ( _openArchives.begin(), _openArchives.end()
                       , [&] (ArchiveEntry const& archive)
                         {
                           return archive.second->hasFile (filename);
                         }
                       );
  }
}

/*
* basic constructor to save the file to project path
*/
MPQFile::MPQFile(std::string const& filename)
  : eof(true)
  , pointer(0)
  , External(false)
  , _filename(noggit::mpq::normalized_filename(filename))
  , _disk_path (getDiskPath (_filename))
{
  if (filename.empty())
    throw std::runtime_error("MPQFile: filename empty");

  if (!noggit::mpq::is_safe_normalized_filename (_filename))
  {
    throw std::invalid_argument ("File '" + filename + "' escapes the project directory.");
  }

  std::ifstream input(_disk_path.string(), std::ios_base::binary | std::ios_base::in);
  if (input.is_open())
  {
    External = true;
    eof = false;

    input.seekg(0, std::ios::end);
    auto const disk_size (input.tellg());
    if (disk_size < 0)
    {
      throw std::runtime_error ("File '" + filename + "' has an unreadable size.");
    }
    static constexpr std::uintmax_t max_file_bytes = 512ULL * 1024ULL * 1024ULL;
    if (static_cast<std::uintmax_t> (disk_size) > max_file_bytes)
    {
      throw std::runtime_error ("File '" + filename + "' exceeds the 512 MiB safety cap.");
    }
    buffer.resize (static_cast<size_t> (disk_size));
    input.seekg(0, std::ios::beg);

    input.read(buffer.data(), buffer.size());

    input.close();
    return;
  }

  std::lock_guard<std::mutex> lock(gMPQFileMutex);

  for (ArchivesMap::reverse_iterator i = _openArchives.rbegin(); i != _openArchives.rend(); ++i)
  {
    HANDLE fileHandle;

    if (!i->second->openFile(filename, &fileHandle))
      continue;

    eof = false;
    {
      std::uint32_t const archived_size (SFileGetFileSize (fileHandle, nullptr));
      static constexpr std::uint32_t max_archive_bytes = 512U * 1024U * 1024U;
      if (archived_size > max_archive_bytes)
      {
        SFileCloseFile (fileHandle);
        throw std::runtime_error ("File '" + filename + "' exceeds the 512 MiB safety cap.");
      }
      buffer.resize (archived_size);
    }
    SFileReadFile(fileHandle, buffer.data(), buffer.size(), nullptr, nullptr); //last nullptrs for newer version of StormLib
    SFileCloseFile(fileHandle);

    return;
  }

  throw std::invalid_argument ("File '" + filename + "' does not exist.");
}

MPQFile::~MPQFile()
{
  close();
}

bool MPQFile::exists (std::string const& filename)
{
  // an empty name would resolve to the project folder itself
  if (filename.empty())
  {
    return false;
  }

  return existsOnDisk (filename) || existsInMPQ (filename);
}
bool MPQFile::existsOnDisk (std::string const& filename)
{
  if (filename.empty())
  {
    return false;
  }

  // normalize and validate, otherwise a name like "../../etc/passwd" would
  // make this an existence oracle for files outside of the project directory
  std::string const normalized (noggit::mpq::normalized_filename (filename));
  if (!noggit::mpq::is_safe_normalized_filename (normalized))
  {
    return false;
  }

  return std::filesystem::exists (getDiskPath (normalized));
}

size_t MPQFile::read(void* dest, size_t bytes)
{
  if (eof || !bytes || pointer >= buffer.size())
    return 0;

  size_t const available = buffer.size() - pointer;
  if (bytes > available)
  {
    bytes = available;
    eof = true;
  }

  std::memcpy(dest, buffer.data() + pointer, bytes);

  // never advance past the end: a read at the end returns 0 and must leave the
  // cursor in a valid position
  pointer += bytes;

  return bytes;
}

bool MPQFile::isEof() const
{
  return eof;
}

void MPQFile::seek(size_t offset)
{
  if (offset > buffer.size())
  {
    throw std::out_of_range ("MPQFile::seek past end of '" + _filename + "'");
  }
  pointer = offset;
  eof = (pointer >= buffer.size());
}

void MPQFile::seekRelative(size_t offset)
{
  if (offset > buffer.size() || pointer > buffer.size() - offset)
  {
    throw std::out_of_range ("MPQFile::seekRelative past end of '" + _filename + "'");
  }
  pointer += offset;
  eof = (pointer >= buffer.size());
}

size_t MPQFile::remaining() const
{
  return pointer >= buffer.size() ? 0U : buffer.size() - pointer;
}

void MPQFile::require_range(size_t offset, size_t bytes) const
{
  if (bytes > buffer.size() || offset > buffer.size() - bytes)
  {
    throw std::out_of_range ("MPQFile::range past end of '" + _filename + "'");
  }
}

char const* MPQFile::getPointerAt(size_t offset) const
{
  require_range(offset, 1U);
  return buffer.data() + offset;
}

void MPQFile::close()
{
  eof = true;
}

size_t MPQFile::getSize() const
{
  return buffer.size();
}

size_t MPQFile::getPos() const
{
  return pointer;
}

char const* MPQFile::getBuffer() const
{
  return buffer.data();
}

char const* MPQFile::getPointer() const
{
  if (pointer > buffer.size())
  {
    throw std::out_of_range ("MPQFile::getPointer past end of '" + _filename + "'");
  }
  return buffer.data() + pointer;
}


void MPQFile::save_file_to_folder(std::string const& folder)
{
  save_file(std::filesystem::path(folder) / _filename);
}

void MPQFile::save_file(std::filesystem::path const& path)
{
  LogDebug << "Save file to: " << path << std::endl;

  auto const directory_name(path.parent_path());
  std::error_code ec;
  std::filesystem::create_directories(directory_name, ec);

  if (ec)
  {
    LogError << "Creating directory \"" << directory_name << "\" failed: " << ec << ". Saving is highly likely to fail." << std::endl;
  }

  std::ofstream output(path.string(), std::ios_base::binary | std::ios_base::out);
  if (output.is_open())
  {
    NOGGIT_LOG << "Saving file \"" << path << "\"." << std::endl;

    output.write(buffer.data(), buffer.size());
    output.close();

    External = true;
  }
}

void MPQFile::SaveFile()
{
  save_file(_disk_path);
}

namespace noggit
{
  namespace mpq
  {
    namespace
    {
      // \todo(c++20): std::iota
      template<typename T, typename It>
      constexpr void constexprIota(It begin, It end, T value)
      {
          for (; begin != end; ++begin, ++value)
          {
              *begin = value;
          }
      }

      constexpr auto build_to_lower_backslash_lookup_table()
      {
          std::array<unsigned char, std::numeric_limits<unsigned char>::max() + 1> r = {};
          constexprIota(r.begin(), r.end(), static_cast<unsigned char>(0));
          for (unsigned char upper = 'A', lower = 'a'; upper <= 'Z'; ++upper, ++lower)
          {
              r[upper] = lower;
          }
          r['\\'] = '/';
          return r;
      }
      constexpr auto build_to_upper_slash_lookup_table()
      {
          std::array<unsigned char, std::numeric_limits<unsigned char>::max() + 1> r = {};
          constexprIota(r.begin(), r.end(), static_cast<unsigned char>(0));
          for (unsigned char upper = 'A', lower = 'a'; upper <= 'Z'; ++upper, ++lower)
          {
              r[lower] = upper;
          }
          r['/'] = '\\';
          return r;
      }

      constexpr auto const to_lower_backslash_lookup_table = build_to_lower_backslash_lookup_table();
      constexpr auto const to_upper_slash_lookup_table = build_to_upper_slash_lookup_table();

      constexpr char to_lower_backslash(char c)
      {
        return to_lower_backslash_lookup_table[static_cast<unsigned char>(c)];
      }
      constexpr char to_upper_slash(char c)
      {
        return to_upper_slash_lookup_table[static_cast<unsigned char>(c)];
      }
    }


    std::string normalized_filename (std::string filename)
    {
      std::transform (filename.begin(), filename.end(), filename.begin(), to_lower_backslash);
      return filename;
    }
    std::string normalized_filename_insane (std::string filename)
    {
      std::transform (filename.begin(), filename.end(), filename.begin(), to_upper_slash);
      return filename;
    }

    bool is_safe_normalized_filename (std::string const& normalized)
    {
      if (normalized.empty() || normalized.size() > 260U)
      {
        return false;
      }

      std::filesystem::path const path (normalized);
      if (path.is_absolute())
      {
        return false;
      }

      for (auto const& part : path)
      {
        auto const text (part.string());
        if (text.empty() || text == "." || text == ".." || text.find (':') != std::string::npos)
        {
          return false;
        }
      }

      return true;
    }
  }
}
