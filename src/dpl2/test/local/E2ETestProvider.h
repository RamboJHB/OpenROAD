// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Data-provider boundary for the production-chain E2E suite.
//
// The repository-local cases in e2e_cases.cpp contain every assertion and call
// FillerRepairEngine.  A provider only creates the requested UDM design,
// exposes the already-wired production Grid/Network, and performs the few
// test mutations needed to create gap/overlap inputs. The adjacent local
// provider supplies the fixture through UDM-compatible test-only types.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "fillerRepair/FillerRepairEngine.h"

namespace eUNL {
class Design;
class PhysDesMgr;
}  // namespace eUNL

namespace dpl2 {
class Grid;
class Network;
}  // namespace dpl2

namespace dpl2::fillerRepair::test {

inline constexpr int kSiteWidth = 1;
inline constexpr int kRowHeight = 8;
inline constexpr int kRowSites = 20;
inline constexpr int kStandardRows = 5;

// Stable semantic roles. Tests compare through these roles instead of assuming
// numeric UDM ids assigned by a particular design loader.
enum class CellRole
{
  Row0ThirdCell,
  Row0TailFiller,
  Row1TailFiller,
  Target,
  TargetLeftFiller,
  TargetRightFiller
};

enum class MasterRole
{
  TargetOld,
  TargetNew,
  RepairFiller,
  ExtraUninstantiatedFiller
};

struct DesignSetup
{
  bool unusedRuleLayers = false;
  bool padRowFirst = false;
  bool padRowLast = false;
  int64_t padRowOriginX = 0;
  std::array<int64_t, kStandardRows> rowOriginX{0, 0, 0, 0, 0};
  bool row0TailHardBlockage = false;
  int row0TailHaloWidth = 0;
};

using PhysicalSnapshot
    = std::vector<std::tuple<int, int64_t, int64_t, int, int, int>>;

class E2ETestDesign
{
 public:
  virtual ~E2ETestDesign() = default;

  virtual eUNL::Design* design() = 0;
  virtual eUNL::PhysDesMgr* desMgr() = 0;
  virtual const eLIB::PhysLibCell& master(MasterRole role) const = 0;
  virtual eUNL::LeafCellID cell(CellRole role) const = 0;
  virtual int64_t rowOriginX(int standardRow) const = 0;
  virtual size_t standardRowCount() const = 0;

  virtual void moveCell(CellRole role, int64_t x, int64_t y) = 0;
  virtual PhysicalSnapshot snapshot() const = 0;
  virtual void activate() = 0;
};

class E2ETestInfrastructure
{
 public:
  virtual ~E2ETestInfrastructure() = default;
  virtual dpl2::Grid* grid() = 0;
  virtual dpl2::Network* network() = 0;
};

class E2ETestProvider
{
 public:
  virtual ~E2ETestProvider() = default;

  // The local provider creates the canonical UDM-compatible fixture.
  virtual std::unique_ptr<E2ETestDesign> createDesign(
      const DesignSetup& setup) = 0;
  virtual std::unique_ptr<E2ETestInfrastructure> createInfrastructure(
      E2ETestDesign& design,
      const DesignSetup& setup) = 0;
};

// Supplied exactly once by the local fake-UDM test runner.
std::unique_ptr<E2ETestProvider> makeE2ETestProvider();

}  // namespace dpl2::fillerRepair::test
