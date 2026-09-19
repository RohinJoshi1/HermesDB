#include "hermesdb/compaction.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hermesdb {

TieredCompactionController::TieredCompactionController(
    TieredCompactionOptions options)
    : options_(std::move(options)) {
  if (options_.num_tiers == 0) {
    throw std::invalid_argument("tiered compaction needs at least one tier");
  }
}

std::optional<TieredCompactionTask>
TieredCompactionController::generate_compaction_task(
    const CompactionState& snapshot) const {
  if (!snapshot.l0_sstables.empty()) {
    throw std::invalid_argument(
        "tiered compaction cannot contain L0 SSTs");
  }
  if (snapshot.levels.size() < options_.num_tiers) {
    return std::nullopt;
  }

  std::size_t newer_size = 0;
  for (std::size_t i = 0; i + 1 < snapshot.levels.size(); ++i) {
    newer_size += snapshot.levels[i].sst_ids.size();
  }
  const double space_amplification =
      static_cast<double>(newer_size) /
      static_cast<double>(snapshot.levels.back().sst_ids.size()) * 100.0;
  if (space_amplification >=
      static_cast<double>(options_.max_size_amplification_percent)) {
    return TieredCompactionTask{
        .tiers = snapshot.levels,
        .bottom_tier_included = true,
    };
  }

  const double ratio_trigger =
      (100.0 + static_cast<double>(options_.size_ratio)) / 100.0;
  std::size_t cumulative_size = 0;
  for (std::size_t i = 0; i + 1 < snapshot.levels.size(); ++i) {
    cumulative_size += snapshot.levels[i].sst_ids.size();
    const double ratio =
        static_cast<double>(snapshot.levels[i + 1].sst_ids.size()) /
        static_cast<double>(cumulative_size);
    // This condition intentionally follows mini-lsm's tier-index check.
    if (ratio > ratio_trigger && i + 1 >= options_.min_merge_width) {
      return TieredCompactionTask{
          .tiers = std::vector<Level>(snapshot.levels.begin(),
                                      snapshot.levels.begin() +
                                          static_cast<std::ptrdiff_t>(i + 1)),
          .bottom_tier_included = false,
      };
    }
  }

  const std::size_t maximum =
      options_.max_merge_width.value_or(
          std::numeric_limits<std::size_t>::max());
  const std::size_t count = std::min(snapshot.levels.size(), maximum);
  return TieredCompactionTask{
      .tiers = std::vector<Level>(snapshot.levels.begin(),
                                  snapshot.levels.begin() +
                                      static_cast<std::ptrdiff_t>(count)),
      .bottom_tier_included = count == snapshot.levels.size(),
  };
}

ApplyResult TieredCompactionController::apply_compaction_result(
    const CompactionState& snapshot, const TieredCompactionTask& task,
    const std::vector<SstId>& output) const {
  if (!snapshot.l0_sstables.empty()) {
    throw std::invalid_argument(
        "tiered compaction cannot contain L0 SSTs");
  }

  ApplyResult result{.state = snapshot, .files_to_remove = {}};
  std::unordered_map<SstId, const std::vector<SstId>*> tiers_to_remove;
  for (const auto& tier : task.tiers) {
    if (!tiers_to_remove.emplace(tier.id, &tier.sst_ids).second) {
      throw std::invalid_argument("duplicate tier id in compaction task");
    }
  }

  std::vector<Level> levels;
  levels.reserve(snapshot.levels.size() + 1);
  bool new_tier_added = false;
  for (const auto& tier : snapshot.levels) {
    const auto selected = tiers_to_remove.find(tier.id);
    if (selected != tiers_to_remove.end()) {
      if (*selected->second != tier.sst_ids) {
        throw std::invalid_argument(
            "tier files changed after task generation");
      }
      result.files_to_remove.insert(result.files_to_remove.end(),
                                    tier.sst_ids.begin(), tier.sst_ids.end());
      tiers_to_remove.erase(selected);
    } else {
      levels.push_back(tier);
    }

    if (tiers_to_remove.empty() && !new_tier_added && !output.empty()) {
      levels.push_back(Level{.id = output.front(), .sst_ids = output});
      new_tier_added = true;
    }
  }
  if (!tiers_to_remove.empty()) {
    throw std::invalid_argument("a task tier is no longer present");
  }
  result.state.levels = std::move(levels);
  return result;
}

}  // namespace hermesdb
