// Minimal in-memory MOCK of odb, covering only the API DplFillerGrid uses.
// Signatures mirror the assumed odb API; this is sandbox scaffolding to
// compile + logic-test the adapter without a full OpenROAD build.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace odb {

struct Rect {
  int xlo=0, ylo=0, xhi=0, yhi=0;
  Rect() = default;
  Rect(int a,int b,int c,int d):xlo(a),ylo(b),xhi(c),yhi(d){}
  int xMin() const {return xlo;} int yMin() const {return ylo;}
  int xMax() const {return xhi;} int yMax() const {return yhi;}
};

struct Point {
  int x_=0, y_=0;
  int x() const { return x_; }
  int y() const { return y_; }
};

enum class dbMasterType { CORE, CORE_SPACER, BLOCK };
enum class dbTechLayerType { ROUTING, IMPLANT, CUT };
enum class dbOrientType { R0, MX, MY, R180 };
enum class dbPlacementStatus { NONE, PLACED, FIXED, LOCKED };
enum class dbSourceType { NONE, DIST, NETLIST };

class dbTechLayer {
 public:
  std::string name_;  dbTechLayerType type_ = dbTechLayerType::ROUTING;
  std::string getName() const { return name_; }
  dbTechLayerType getType() const { return type_; }
};

class dbBox {
 public:
  Rect box_;  dbTechLayer* layer_ = nullptr;
  Rect getBox() const { return box_; }
  dbTechLayer* getTechLayer() const { return layer_; }
};

class dbSite {
 public:
  int w_=1, h_=1;
  int getWidth() const { return w_; }
  int getHeight() const { return h_; }
};

class dbMaster {
 public:
  std::string name_;  dbMasterType type_ = dbMasterType::CORE;
  bool block_=false;  int w_=0, h_=0;  std::vector<dbBox*> obs_;
  std::string getName() const { return name_; }
  dbMasterType getType() const { return type_; }
  bool isBlock() const { return block_; }
  int getWidth() const { return w_; }
  int getHeight() const { return h_; }
  std::vector<dbBox*> getObstructions() const { return obs_; }
};

class dbBlock;

class dbInst {
 public:
  dbBlock* block_=nullptr; dbMaster* master_=nullptr; std::string name_;
  int x_=0, y_=0; bool placed_=true, fixed_=false, destroyed_=false;
  dbOrientType orient_=dbOrientType::R0;
  dbPlacementStatus status_=dbPlacementStatus::PLACED;
  dbSourceType source_=dbSourceType::NONE;
  dbBox bbox_;
  dbMaster* getMaster() const { return master_; }
  bool isPlaced() const { return placed_; }
  bool isFixed() const { return fixed_; }
  dbBox* getBBox() {
    bbox_.box_ = Rect(x_, y_, x_ + (master_?master_->w_:0),
                      y_ + (master_?master_->h_:0));
    return &bbox_;
  }
  void setOrient(dbOrientType o){orient_=o;}
  void setLocation(int x,int y){x_=x;y_=y;}
  void setPlacementStatus(dbPlacementStatus s){status_=s;}
  void setSourceType(dbSourceType s){source_=s;}
  static dbInst* create(dbBlock* b, dbMaster* m, const char* nm, bool physical_only);
  static void destroy(dbInst* i);
};

class dbRow {
 public:
  dbSite* site_=nullptr; int ox_=0, oy_=0; dbOrientType orient_=dbOrientType::R0;
  dbSite* getSite() const { return site_; }
  Point getOrigin() const { return Point{ox_, oy_}; }
  dbOrientType getOrient() const { return orient_; }
};

class dbBlock {
 public:
  Rect core_;  std::vector<dbRow*> rows_;  std::vector<dbInst*> insts_;
  Rect getCoreArea() const { return core_; }
  std::vector<dbRow*> getRows() const { return rows_; }
  std::vector<dbInst*> getInsts() const { return insts_; }
};

inline dbInst* dbInst::create(dbBlock* b, dbMaster* m, const char* nm, bool){
  dbInst* i = new dbInst(); i->block_=b; i->master_=m; i->name_=nm;
  b->insts_.push_back(i); return i;
}
inline void dbInst::destroy(dbInst* i){
  i->destroyed_=true;
  auto& v = i->block_->insts_;
  v.erase(std::remove(v.begin(), v.end(), i), v.end());
}

}  // namespace odb
