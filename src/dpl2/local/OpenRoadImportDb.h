#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace odb {
class dbDatabase;
}

namespace dpl2::local {

// Import the active OpenROAD ODB block into the test-only UDM-compatible
// backing store, then construct the real DePlace Grid/Network over that data.
// ODB is read-only throughout this operation.
bool importOpenRoadDb(odb::dbDatabase* db,
                      const std::vector<std::string>& fillerMasters,
                      std::ostream& out);

}  // namespace dpl2::local
