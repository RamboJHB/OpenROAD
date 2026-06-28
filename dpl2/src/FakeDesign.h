// FakeDesign: in-memory DesignIO for tests and as a porting reference.
//
// It shows exactly what a real port must provide: a site grid (kind + VT +
// owning filler id), a filler table (rectangles), a same-size master lookup,
// the violation list, and the two write ops (relabel VT in place / report
// unfixable).  A real adapter implements the same DesignIO methods against the
// production database instead of these vectors.
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "VtRepair.h"

namespace vtrepair {

class FakeDesign : public DesignIO
{
 public:
  // ---- build helpers ----
  void resize(int rows, int cols)
  {
    rows_ = rows;
    cols_ = cols;
    kind_.assign(rows, std::vector<SiteKind>(cols, SiteKind::Empty));
    vt_.assign(rows, std::vector<Vt>(cols, VT_NONE));
    owner_.assign(rows, std::vector<FillerId>(cols, NO_FILLER));
  }
  // Paint a cell (fixed) rectangle.
  void addCell(int row, int col, int w, int h, const Vt& vt)
  {
    paint(row, col, w, h, SiteKind::Cell, vt, NO_FILLER);
  }
  // Add a filler (changeable) rectangle; returns its id.
  FillerId addFiller(int row, int col, int w, int h, const Vt& vt)
  {
    const FillerId id = static_cast<FillerId>(fillers_.size());
    fillers_.push_back(FillerBox{row, col, w, h, vt});
    paint(row, col, w, h, SiteKind::Filler, vt, id);
    return id;
  }
  void addMaster(const Vt& vt, int w, int h)
  {
    masters_.insert(std::make_tuple(vt, w, h));
  }
  void addViolation(int id, int row, int col, const std::string& type)
  {
    viols_.push_back(Violation{id, row, col, type});
  }

  // ---- DesignIO: READ ----
  std::vector<Violation> readViolations() override { return viols_; }
  int numRows() const override { return rows_; }
  int numCols(int) const override { return cols_; }
  SiteKind kindAt(int r, int c) const override { return kind_[r][c]; }
  Vt vtAt(int r, int c) const override { return vt_[r][c]; }
  FillerId fillerIdAt(int r, int c) const override { return owner_[r][c]; }
  FillerBox fillerBox(FillerId id) const override { return fillers_[id]; }
  std::vector<Vt> candidateVts() const override
  {
    std::vector<Vt> out;
    for (const auto& m : masters_) {
      const Vt& v = std::get<0>(m);
      if (std::find(out.begin(), out.end(), v) == out.end()) {
        out.push_back(v);
      }
    }
    return out;
  }
  bool masterExists(const Vt& vt, int w, int h) const override
  {
    return masters_.count(std::make_tuple(vt, w, h)) > 0;
  }

  // ---- DesignIO: WRITE ----
  void replaceFillerVt(FillerId id, const Vt& newVt) override
  {
    FillerBox& b = fillers_[id];
    b.vt = newVt;
    for (int r = b.row; r < b.row + b.height; ++r) {
      for (int c = b.col; c < b.col + b.width; ++c) {
        vt_[r][c] = newVt;  // kind/owner unchanged: same instance, new VT
      }
    }
  }
  void reportUnfixable(const Violation& v, const std::string& reason) override
  {
    unfixable_.push_back({v, reason});
  }

  // ---- test inspection ----
  const FillerBox& filler(FillerId id) const { return fillers_[id]; }
  const std::vector<std::pair<Violation, std::string>>& unfixable() const
  {
    return unfixable_;
  }

 private:
  void
  paint(int row, int col, int w, int h, SiteKind k, const Vt& vt, FillerId id)
  {
    for (int r = row; r < row + h; ++r) {
      for (int c = col; c < col + w; ++c) {
        kind_[r][c] = k;
        vt_[r][c] = vt;
        owner_[r][c] = id;
      }
    }
  }

  int rows_ = 0, cols_ = 0;
  std::vector<std::vector<SiteKind>> kind_;
  std::vector<std::vector<Vt>> vt_;
  std::vector<std::vector<FillerId>> owner_;
  std::vector<FillerBox> fillers_;
  std::set<std::tuple<Vt, int, int>> masters_;  // available (vt,w,h)
  std::vector<Violation> viols_;
  std::vector<std::pair<Violation, std::string>> unfixable_;
};

}  // namespace vtrepair
