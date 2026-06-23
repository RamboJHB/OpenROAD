// Logic test for DplFillerGrid against the odb MOCK.
//   g++ -std=c++17 -I <repo>/src/dpl/src -I src/dpl/test/filler_repair_mock
//       <repo>/src/dpl/src/DplFillerGrid.cpp
//       <repo>/src/dpl/src/FillerRepair.cpp
//       src/dpl/test/dpl_filler_grid_test.cpp -o /tmp/dpl_test && /tmp/dpl_test
#include <iostream>
#include <set>
#include <vector>

#include "DplFillerGrid.h"
#include "odb/db.h"
#include "utl/Logger.h"

using namespace odb;
using dpl_fr::repairDirtyFillers;

static int g_pass = 0, g_fail = 0;
static void check(bool c, const std::string& m)
{
  if (c) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cout << "  FAIL: " << m << "\n";
  }
}

// ---- mock design builders --------------------------------------------------
static const int SW = 10;  // site width (DBU)
static const int RH = 20;  // row height (DBU)

static dbSite* g_site = nullptr;

static dbMaster* makeMaster(const std::string& name,
                            dbMasterType type,
                            const std::string& vt,
                            int width_sites,
                            int height_rows)
{
  auto* m = new dbMaster();
  m->name_ = name;
  m->type_ = type;
  m->w_ = width_sites * SW;
  m->h_ = height_rows * RH;
  if (!vt.empty()) {
    auto* layer = new dbTechLayer();
    layer->name_ = vt;
    layer->type_ = dbTechLayerType::IMPLANT;
    auto* box = new dbBox();
    box->layer_ = layer;
    box->box_ = Rect(0, 0, m->w_, m->h_);
    m->obs_.push_back(box);
  }
  return m;
}

static dbInst* place(dbBlock* b,
                     dbMaster* m,
                     int col,
                     int row,
                     bool dirtyKind = false)
{
  auto* i = new dbInst();
  i->block_ = b;
  i->master_ = m;
  i->name_ = m->name_ + "_" + std::to_string(row) + "_" + std::to_string(col);
  i->x_ = col * SW;
  i->y_ = row * RH;
  i->placed_ = true;
  b->insts_.push_back(i);
  (void) dirtyKind;
  return i;
}

static dbBlock* makeBlock(int cols, int rows)
{
  auto* b = new dbBlock();
  b->core_ = Rect(0, 0, cols * SW, rows * RH);
  for (int r = 0; r < rows; ++r) {
    auto* row = new dbRow();
    row->site_ = g_site;
    row->ox_ = 0;
    row->oy_ = r * RH;
    row->orient_ = (r % 2 == 0) ? dbOrientType::R0 : dbOrientType::MX;
    b->rows_.push_back(row);
  }
  return b;
}

static int countFillerInsts(dbBlock* b)
{
  int n = 0;
  for (auto* i : b->insts_) {
    if (i->master_->type_ == dbMasterType::CORE_SPACER) {
      ++n;
    }
  }
  return n;
}

int main()
{
  utl::Logger logger;
  g_site = new dbSite();
  g_site->w_ = SW;
  g_site->h_ = RH;

  // VT-L cell master (2 sites) and a set of L fillers (2/4/6 sites, h1) +
  // a height-2 L filler (2 sites).
  dbMaster* cellL = makeMaster("CELL_L", dbMasterType::CORE, "L", 2, 1);
  dbMaster* f2 = makeMaster("FILL_L2", dbMasterType::CORE_SPACER, "L", 2, 1);
  dbMaster* f4 = makeMaster("FILL_L4", dbMasterType::CORE_SPACER, "L", 4, 1);
  dbMaster* f6 = makeMaster("FILL_L6", dbMasterType::CORE_SPACER, "L", 6, 1);
  dbMaster* f2h2
      = makeMaster("FILL_L2H2", dbMasterType::CORE_SPACER, "L", 2, 2);
  std::vector<dbMaster*> lib = {f6, f4, f2, f2h2};

  // ===== Case 1: single row, dirty 6-site window between two L cells =====
  {
    dbBlock* b = makeBlock(/*cols=*/10, /*rows=*/1);
    place(b, cellL, /*col=*/0, /*row=*/0);               // cols 0-1
    dbInst* dirty = place(b, f6, /*col=*/2, /*row=*/0);  // cols 2-7 (6 sites)
    place(b, cellL, /*col=*/8, /*row=*/0);               // cols 8-9
    std::set<dbInst*> dirtySet = {dirty};

    auto r = repairDirtyFillers(b,
                                dirtySet,
                                lib,
                                /*preserve=*/false,
                                /*min_w=*/1,
                                &logger);
    check(r.unsolved.empty(), "C1 solvable");
    check(dirty->destroyed_, "C1 dirty filler destroyed");
    int sum = 0;
    bool allL = true;
    for (auto& p : r.placed) {
      sum += p.width;
      allL &= (p.vt == "L");
      allL &= (p.height == 1);
    }
    check(sum == 6, "C1 placed widths sum to 6");
    check(allL, "C1 all placed fillers are L, height 1");
    check(countFillerInsts(b) >= 1, "C1 new filler instances created");
  }

  // ===== Case 2: multi-height, 2-row x 4-site dirty rectangle =====
  {
    dbBlock* b = makeBlock(/*cols=*/8, /*rows=*/2);
    dbMaster* f4h2
        = makeMaster("FILL_L4H2", dbMasterType::CORE_SPACER, "L", 4, 2);
    std::vector<dbMaster*> lib2 = {f4h2, f2h2};
    place(b, cellL, 0, 0);
    place(b, cellL, 0, 1);
    dbInst* d0 = place(b,
                       makeMaster("DF0", dbMasterType::CORE_SPACER, "L", 4, 1),
                       2,
                       0);  // row0 cols 2-5
    dbInst* d1 = place(b,
                       makeMaster("DF1", dbMasterType::CORE_SPACER, "L", 4, 1),
                       2,
                       1);  // row1 cols 2-5
    place(b, cellL, 6, 0);
    place(b, cellL, 6, 1);
    std::set<dbInst*> dirtySet = {d0, d1};

    auto r = repairDirtyFillers(b, dirtySet, lib2, false, 1, &logger);
    check(r.unsolved.empty(), "C2 multi-height solvable");
    check(d0->destroyed_ && d1->destroyed_, "C2 both dirty rows destroyed");
    bool hasH2 = false;
    for (auto& p : r.placed) {
      if (p.height == 2) {
        hasH2 = true;
      }
    }
    check(hasH2, "C2 placed a height-2 filler spanning both rows");
  }

  // ===== Case 3: unsolvable (1-site window, no 1-site filler) =====
  {
    dbBlock* b = makeBlock(/*cols=*/7, /*rows=*/1);
    place(b, cellL, 0, 0);  // cols 0-1
    dbInst* dirty
        = place(b,
                makeMaster("DF", dbMasterType::CORE_SPACER, "L", 1, 1),
                3,
                0);         // 1-site dirty at col 3
    place(b, cellL, 5, 0);  // cols 5-6
    std::set<dbInst*> dirtySet = {dirty};

    auto r = repairDirtyFillers(b, dirtySet, lib, false, 1, &logger);
    check(r.unsolved.size() == 1, "C3 one unsolved window");
    check(r.placed.empty(), "C3 nothing placed");
    check(dirty->destroyed_, "C3 dirty still removed");
  }

  std::cout << "DplFillerGrid mock test: " << g_pass << " passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
