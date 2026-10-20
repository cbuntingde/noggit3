// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/client_profile.hpp>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/MPQ.h>

#include <cstdint>
#include <string>

DBCFile::DBCFile(const std::string& _filename)
  : filename(_filename)
{}

bool DBCFile::is_vanilla()
{
  return noggit::client_profile::current() == noggit::client_profile::client_version::vanilla_1_12_1;
}

void DBCFile::open()
{
  MPQFile f (filename);

  if (f.isEof())
  {
    LogError << "The DBC file \"" << filename << "\" could not be opened. This application may crash soon as the file is most likely needed." << std::endl;
    return;
  }
  LogDebug << "Opening DBC \"" << filename << "\"" << std::endl;

  if (f.getSize() < 20U)
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" is truncated.");
  }

  char header[4];
  uint32_t file_record_count = 0;
  uint32_t file_field_count = 0;
  uint32_t file_record_size = 0;
  uint32_t file_string_size = 0;

  if (f.read(header, 4) != 4
    || f.read(&file_record_count, 4) != 4
    || f.read(&file_field_count, 4) != 4
    || f.read(&file_record_size, 4) != 4
    || f.read(&file_string_size, 4) != 4)
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has a truncated header.");
  }

  if (!(header[0] == 'W' && header[1] == 'D' && header[2] == 'B' && header[3] == 'C'))
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has an invalid magic.");
  }

  if (file_field_count == 0 || file_record_size == 0)
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has an invalid layout.");
  }

  if (static_cast<std::uint64_t>(file_field_count) * 4ULL != file_record_size)
  {
    throw std::logic_error ("non four-byte-columns not supported");
  }

  static constexpr std::uint64_t max_records = 1'000'000ULL;
  static constexpr std::uint64_t max_record_size = 4096ULL;
  static constexpr std::uint64_t max_string_size = 64ULL * 1024ULL * 1024ULL;
  std::uint64_t const data_bytes = static_cast<std::uint64_t>(file_record_size) * file_record_count;
  if (file_record_count > max_records || file_record_size > max_record_size || data_bytes > max_string_size
    || file_string_size > max_string_size)
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has implausible sizes.");
  }

  if (20ULL + data_bytes + file_string_size > f.getSize())
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" is truncated.");
  }

  recordCount = file_record_count;
  fieldCount = file_field_count;
  recordSize = file_record_size;
  stringSize = file_string_size;

  locale_count = is_vanilla() ? 8 : 9;

  data.resize (static_cast<size_t>(data_bytes));
  if (!data.empty() && f.read (data.data(), data.size()) != data.size())
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has truncated records.");
  }

  stringTable.resize (stringSize);
  if (!stringTable.empty() && f.read (stringTable.data(), stringTable.size()) != stringTable.size())
  {
    throw std::runtime_error ("DBC file \"" + filename + "\" has a truncated string table.");
  }

  f.close();
}
