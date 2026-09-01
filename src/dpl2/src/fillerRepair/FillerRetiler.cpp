// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRetiler.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace dpl2::fillerRepair::internal {
namespace {

uint64_t siteKey(RowId rowId, int colId)
{
  return (static_cast<uint64_t>(static_cast<uint32_t>(rowId)) << 32)
         | static_cast<uint32_t>(colId);
}

class ExactCoverSearch
{
 public:
  ExactCoverSearch(std::vector<SiteCell> sites,
                   std::vector<FillerFootprint> footprints,
                   RetileConfig config)
      : sites_(std::move(sites)),
        footprints_(std::move(footprints)),
        config_(config),
        covered_(sites_.size(), false)
  {
    for (std::size_t index = 0; index < sites_.size(); ++index) {
      index_by_site_.emplace(
          siteKey(sites_[index].rowId, sites_[index].colId), index);
    }
  }

  RetileResult run()
  {
    if (config_.maxSolutions == 0 || config_.maxSearchStates == 0) {
      result_.truncated = !sites_.empty();
      return result_;
    }
    search(0);
    return result_;
  }

 private:
  void search(std::size_t coveredCount)
  {
    if (result_.solutions.size() >= config_.maxSolutions) {
      result_.truncated = true;
      return;
    }
    if (result_.searchStates >= config_.maxSearchStates) {
      result_.truncated = true;
      return;
    }
    ++result_.searchStates;
    if (coveredCount == sites_.size()) {
      result_.solutions.push_back(current_);
      return;
    }

    std::size_t first = 0;
    while (first < covered_.size() && covered_[first]) {
      ++first;
    }
    if (first == covered_.size()) {
      return;
    }

    const SiteCell anchor = sites_[first];
    for (const FillerFootprint& footprint : footprints_) {
      std::vector<std::size_t> cells;
      cells.reserve(static_cast<std::size_t>(footprint.widthSites)
                    * static_cast<std::size_t>(footprint.heightRows));
      bool fits = true;
      for (int rowOffset = 0; rowOffset < footprint.heightRows && fits;
           ++rowOffset) {
        for (int colOffset = 0; colOffset < footprint.widthSites;
             ++colOffset) {
          const auto found = index_by_site_.find(
              siteKey(anchor.rowId + rowOffset, anchor.colId + colOffset));
          if (found == index_by_site_.end() || covered_[found->second]) {
            fits = false;
            break;
          }
          cells.push_back(found->second);
        }
      }
      if (!fits) {
        continue;
      }

      for (const std::size_t cell : cells) {
        covered_[cell] = true;
      }
      current_.push_back(
          TiledFiller{footprint.masterId, anchor.rowId, anchor.colId});
      search(coveredCount + cells.size());
      current_.pop_back();
      for (const std::size_t cell : cells) {
        covered_[cell] = false;
      }
      if (result_.solutions.size() >= config_.maxSolutions
          || result_.searchStates >= config_.maxSearchStates) {
        result_.truncated = true;
        return;
      }
    }
  }

  std::vector<SiteCell> sites_;
  std::vector<FillerFootprint> footprints_;
  RetileConfig config_;
  std::unordered_map<uint64_t, std::size_t> index_by_site_;
  std::vector<bool> covered_;
  std::vector<TiledFiller> current_;
  RetileResult result_;
};

}  // namespace

RetileResult enumerateRetilings(std::vector<SiteCell> emptySites,
                                std::vector<FillerFootprint> footprints,
                                RetileConfig config)
{
  std::sort(emptySites.begin(), emptySites.end());
  emptySites.erase(std::unique(emptySites.begin(), emptySites.end()),
                   emptySites.end());
  footprints.erase(
      std::remove_if(footprints.begin(),
                     footprints.end(),
                     [](const FillerFootprint& footprint) {
                       return footprint.widthSites <= 0
                              || footprint.heightRows <= 0;
                     }),
      footprints.end());
  std::sort(footprints.begin(),
            footprints.end(),
            [](const FillerFootprint& left, const FillerFootprint& right) {
              const int leftArea = left.widthSites * left.heightRows;
              const int rightArea = right.widthSites * right.heightRows;
              if (leftArea != rightArea) {
                return leftArea > rightArea;
              }
              if (left.heightRows != right.heightRows) {
                return left.heightRows > right.heightRows;
              }
              if (left.widthSites != right.widthSites) {
                return left.widthSites > right.widthSites;
              }
              return left.masterId < right.masterId;
            });
  footprints.erase(
      std::unique(footprints.begin(),
                  footprints.end(),
                  [](const FillerFootprint& left,
                     const FillerFootprint& right) {
                    return left.widthSites == right.widthSites
                           && left.heightRows == right.heightRows;
                  }),
      footprints.end());
  return ExactCoverSearch(
             std::move(emptySites), std::move(footprints), config)
      .run();
}

}  // namespace dpl2::fillerRepair::internal
