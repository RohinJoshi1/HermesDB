#include "hermesdb/compaction.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace hermesdb {
namespace {

void require_levels(const CompactionState& state, std::size_t count) {
  if (state.levels.size() < count) {
    throw std::invalid_argument("compaction state has too few levels");
  }
}

void require_same(const std::vector<SstId>& expected,
                  const std::vector<SstId>& actual) {
  if (expected != actual) {
    throw std::invalid_argument("SST layout changed after task generation");
  }
}

}  // namespace

SimpleLeveledCompactionController::SimpleLeveledCompactionController(
    SimpleLeveledCompactionOptions options)
    : options_(std::move(options)) {}

std::optional<SimpleLeveledCompactionTask>
SimpleLeveledCompactionController::generate_compaction_task(
    const CompactionState& snapshot) const {
  if (options_.max_levels == 0) {
    return std::nullopt;
  }
  require_levels(snapshot, options_.max_levels);

  std::vector<std::size_t> level_sizes;
  level_sizes.reserve(options_.max_levels + 1);
  level_sizes.push_back(snapshot.l0_sstables.size());
  for (std::size_t i = 0; i < options_.max_levels; ++i) {
    level_sizes.push_back(snapshot.levels[i].sst_ids.size());
  }

  if (snapshot.l0_sstables.size() >=
      options_.level0_file_num_compaction_trigger) {
    return SimpleLeveledCompactionTask{
        .upper_level = std::nullopt,
        .upper_level_sst_ids = snapshot.l0_sstables,
        .lower_level = 1,
        .lower_level_sst_ids = snapshot.levels[0].sst_ids,
        .is_lower_level_bottom_level = false,
    };
  }

  for (std::size_t upper = 1; upper < options_.max_levels; ++upper) {
    const std::size_t lower = upper + 1;
    const double ratio = static_cast<double>(level_sizes[lower]) /
                         static_cast<double>(level_sizes[upper]);
    if (ratio <
        static_cast<double>(options_.size_ratio_percent) / 100.0) {
      return SimpleLeveledCompactionTask{
          .upper_level = upper,
          .upper_level_sst_ids = snapshot.levels[upper - 1].sst_ids,
          .lower_level = lower,
          .lower_level_sst_ids = snapshot.levels[lower - 1].sst_ids,
          .is_lower_level_bottom_level = lower == options_.max_levels,
      };
    }
  }
  return std::nullopt;
}

ApplyResult SimpleLeveledCompactionController::apply_compaction_result(
    const CompactionState& snapshot,
    const SimpleLeveledCompactionTask& task,
    const std::vector<SstId>& output) const {
  if (task.lower_level == 0 ||
      task.lower_level > snapshot.levels.size()) {
    throw std::invalid_argument("invalid lower level in compaction task");
  }

  ApplyResult result{.state = snapshot, .files_to_remove = {}};
  if (task.upper_level) {
    if (*task.upper_level == 0 ||
        *task.upper_level > result.state.levels.size()) {
      throw std::invalid_argument("invalid upper level in compaction task");
    }
    auto& upper = result.state.levels[*task.upper_level - 1].sst_ids;
    require_same(task.upper_level_sst_ids, upper);
    result.files_to_remove.insert(result.files_to_remove.end(), upper.begin(),
                                  upper.end());
    upper.clear();
  } else {
    result.files_to_remove.insert(result.files_to_remove.end(),
                                  task.upper_level_sst_ids.begin(),
                                  task.upper_level_sst_ids.end());
    std::unordered_set<SstId> compacted(task.upper_level_sst_ids.begin(),
                                        task.upper_level_sst_ids.end());
    std::vector<SstId> retained;
    retained.reserve(result.state.l0_sstables.size());
    for (SstId id : result.state.l0_sstables) {
      if (compacted.erase(id) == 0) {
        retained.push_back(id);
      }
    }
    if (!compacted.empty()) {
      throw std::invalid_argument("an L0 task SST is no longer present");
    }
    result.state.l0_sstables = std::move(retained);
  }

  auto& lower = result.state.levels[task.lower_level - 1].sst_ids;
  require_same(task.lower_level_sst_ids, lower);
  result.files_to_remove.insert(result.files_to_remove.end(), lower.begin(),
                                lower.end());
  lower = output;
  return result;
}

}  // namespace hermesdb
