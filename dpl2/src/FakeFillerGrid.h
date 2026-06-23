// FakeFillerGrid: a throwaway in-memory implementation of FillerGrid.
//
// Purpose: drive FillerRepair in unit tests without any database.  To port the
// repair to a real database (OpenROAD dpl grid_, or another DB), implement the
// FillerGrid interface against that DB instead of using this fake -- the
// FillerRepair algorithm does not change.
//
// Model: rows[r][c] is one site.  A height-h filler occupies rows [r, r+h).
#pragma once

#include <string>
#include <vector>

#include "FillerRepair.h"

namespace dpl_fr {

struct FakeSite
{
  SiteKind kind = SiteKind::Empty;
  Vt vt = VT_NONE;
};

class FakeFillerGrid : public FillerGrid
{
 public:
  std::vector<std::vector<FakeSite>> rows;

  int numRows() const override { return static_cast<int>(rows.size()); }
  int numCols(int row) const override
  {
    return static_cast<int>(rows[row].size());
  }
  SiteKind kindAt(int row, int col) const override
  {
    return rows[row][col].kind;
  }
  Vt vtAt(int row, int col) const override { return rows[row][col].vt; }

  void clearSite(int row, int col) override
  {
    rows[row][col] = FakeSite{SiteKind::Empty, VT_NONE};
  }
  void placeFiller(const PlacedFiller& f) override
  {
    for (int rr = f.row; rr < f.row + f.height; ++rr) {
      for (int cc = f.col; cc < f.col + f.width; ++cc) {
        rows[rr][cc] = FakeSite{SiteKind::CleanFiller, f.vt};
      }
    }
  }

  // ---- test helpers ----
  void addRow(const std::vector<FakeSite>& row) { rows.push_back(row); }
};

}  // namespace dpl_fr
