// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <string>

namespace noggit
{
  // Headless ADT round-trip check: loads every tile of a map, saves it
  // (via the vanilla mclq export path) and loads the saved file again.
  // Everything is written to <working dir>/adt_roundtrip/<map>, the real
  // project folder is left untouched. Pass tile_x/tile_z >= 0 to only
  // process a single tile. Returns 0 if every tile round-tripped.
  int run_adt_roundtrip ( std::string const& map_name
                        , int map_id
                        , int tile_x = -1
                        , int tile_z = -1
                        );
}
