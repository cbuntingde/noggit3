// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cassert>
#include <string>
#include <vector>
#include <stdexcept>

class DBCFile
{
public:
  explicit DBCFile(const std::string& filename);

  // Open database. It must be openened before it can be used.
  void open();

  class NotFound : public std::runtime_error
  {
  public:
    NotFound() : std::runtime_error("Key was not found")
    { }
  };

  class Iterator;
  class Record
  {
  public:
    const float& getFloat(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<float*>(offset + field * 4);
    }
    const unsigned int& getUInt(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<unsigned int*>(offset + field * 4);
    }
    const int& getInt(size_t field) const
    {
      assert(field < file.fieldCount);
      return *reinterpret_cast<int*>(offset + field * 4);
    }
    const char *getString(size_t field) const
    {
      assert(field < file.fieldCount);
      size_t stringOffset = getUInt(field);
      assert(stringOffset < file.stringSize);
      return file.stringTable.data() + stringOffset;
    }
    const char *getLocalizedString(size_t field, int locale = -1) const
    {
      int loc = locale;
      if (locale == -1)
      {
        // vanilla's LocalizedString is 8 locale strings followed by a flags
        // word, so the flags slot must not be read as a string offset
        int const locale_slots = static_cast<int>(file.locale_count);
        assert(field + locale_slots <= file.fieldCount);

        for (loc = 0; loc < locale_slots; loc++)
        {
          if (getUInt(field + loc) != 0)
            break;
        }

        // no locale populated: the trailing flags word is not a string
        if (loc == locale_slots)
        {
          return "";
        }
      }

      assert(field + loc < file.fieldCount);
      size_t stringOffset = getUInt(field + loc);
      assert(stringOffset < file.stringSize);
      return file.stringTable.data() + stringOffset;
    }
  private:
    Record(const DBCFile &pfile, unsigned char *poffset) : file(pfile), offset(poffset) {}
    const DBCFile &file;
    unsigned char *offset;

    friend class DBCFile;
    friend class DBCFile::Iterator;
  };
  /** Iterator that iterates over records
  */
  class Iterator
  {
  public:
    Iterator(const DBCFile &file, unsigned char *offset) :
      record(file, offset) {}
    /// Advance (prefix only)
    Iterator & operator++() {
      record.offset += record.file.recordSize;
      return *this;
    }
    /// Return address of current instance
    Record const & operator*() const { return record; }
    const Record* operator->() const {
      return &record;
    }
    /// Comparison
    bool operator==(const Iterator &b) const
    {
      return record.offset == b.record.offset;
    }
    bool operator!=(const Iterator &b) const
    {
      return record.offset != b.record.offset;
    }
  private:
    Record record;
  };

  inline Record getRecord(size_t id)
  {
    return Record(*this, data.data() + id*recordSize);
  }

  inline Iterator begin()
  {
    return Iterator(*this, data.data());
  }
  inline Iterator end()
  {
    return Iterator(*this, data.data() + data.size());
  }

  inline size_t getRecordCount() const { return recordCount; }
  inline size_t getFieldCount() const { return fieldCount; }

  // whether the active client profile is vanilla, whose dbc layouts differ
  static bool is_vanilla();
  inline Record getByID(unsigned int id, size_t field = 0)
  {
    for (Iterator i = begin(); i != end(); ++i)
    {
      if (i->getUInt(field) == id)
        return (*i);
    }
    throw NotFound();
  }

private:
  std::string filename;
  size_t recordSize;
  size_t recordCount;
  size_t fieldCount;
  size_t stringSize;
  // locale strings a LocalizedString holds before its flags word;
  // vanilla has 8, later clients have more
  size_t locale_count = 9;
  std::vector<unsigned char> data;
  std::vector<char> stringTable;
};
