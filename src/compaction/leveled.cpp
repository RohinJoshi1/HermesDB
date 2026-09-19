#include "hermesdb/compaction.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace hermesdb {
namespace {

const SstMetadata& metadata(const CompactionState& state, SstId id) {
  const auto found = state.sstables.find(id);
  if (found == state.sstables.end()) {
    throw std::invalid_argument("missing SST metadata");
  }
  return found->second;
}

std::size_t megabytes_to_bytes(std::size_t megabytes) {
  constexpr std::size_t kMiB = 1024 * 1024;
  if (megabytes > std::numeric_limits<std::size_t>::max() / kMiB) {
    throw std::invalid_argument("base level size overflows size_t");
  }
  return megabytes * kMiB;
}

}  // namespace

LeveledCompactionController::LeveledCompactionController(
    LeveledCompactionOptions options)
    : options_(std::move(options)) {
  if (options_.max_levels == 0) {
    throw std::invalid_argument("leveled compaction needs at least one level");
  }
  if (options_.level_size_multiplier == 0) {
    throw std::invalid_argument("level size multiplier must be nonzero");
  }
}

std::vector<SstId> LeveledCompactionController::find_overlapping_ssts(
    const CompactionState& snapshot, const std::vector<SstId>& sst_ids,
    std::size_t in_level) const {
  if (sst_ids.empty()) {
    throw std::invalid_argument("cannot find overlap for an empty SST set");
  }
  if (in_level == 0 || in_level > snapshot.levels.size()) {
    throw std::invalid_argument("overlap level is outside the state");
  }

  std::string begin_key = metadata(snapshot, sst_ids.front()).first_key;
  std::string end_key = metadata(snapshot, sst_ids.front()).last_key;
  for (SstId id : sst_ids) {
    const auto& sst = metadata(snapshot, id);
    begin_key = std::min(begin_key, sst.first_key);
    end_key = std::max(end_key, sst.last_key);
  }

  std::vector<SstId> overlaps;
  for (SstId id : snapshot.levels[in_level - 1].sst_ids) {
    const auto& sst = metadata(snapshot, id);
    if (!(sst.last_key < begin_key || sst.first_key > end_key)) {
      overlaps.push_back(id);
    }
  }
  return overlaps;
}

std::optional<LeveledCompactionTask>
LeveledCompactionController::generate_compaction_task(
    const CompactionState& snapshot) const {
  if (snapshot.levels.size() < options_.max_levels) {
    throw std::invalid_argument("compaction state has too few levels");
  }

  std::vector<std::size_t> targets(options_.max_levels, 0);
  std::vector<std::size_t> actual(options_.max_levels, 0);
  std::size_t base_level = options_.max_levels;
  for (std::size_t i = 0; i < options_.max_levels; ++i) {
    for (SstId id : snapshot.levels[i].sst_ids) {
      const std::size_t size = metadata(snapshot, id).table_size;
      if (size > std::numeric_limits<std::size_t>::max() - actual[i]) {
        throw std::overflow_error("level size overflows size_t");
      }
      actual[i] += size;
    }
  }

  const std::size_t base_size = megabytes_to_bytes(options_.base_level_size_mb);
  targets.back() = std::max(actual.back(), base_size);
  for (std::size_t i = options_.max_levels - 1; i-- > 0;) {
    const std::size_t next = targets[i + 1];
    if (next > base_size) {
      targets[i] = next / options_.level_size_multiplier;
    }
    if (targets[i] > 0) {
      base_level = i + 1;
    }
  }

  if (snapshot.l0_sstables.size() >=
      options_.level0_file_num_compaction_trigger) {
    return LeveledCompactionTask{
        .upper_level = std::nullopt,
        .upper_level_sst_ids = snapshot.l0_sstables,
        .lower_level = base_level,
        .lower_level_sst_ids =
            find_overlapping_ssts(snapshot, snapshot.l0_sstables, base_level),
        .is_lower_level_bottom_level = base_level == options_.max_levels,
    };
  }

  std::vector<std::pair<double, std::size_t>> priorities;
  for (std::size_t i = 0; i < options_.max_levels; ++i) {
    const double priority =
        static_cast<double>(actual[i]) / static_cast<double>(targets[i]);
    if (priority > 1.0) {
      priorities.emplace_back(priority, i + 1);
    }
  }
  std::sort(priorities.begin(), priorities.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first > rhs.first;
            });
  if (priorities.empty()) {
    return std::nullopt;
  }

  const std::size_t upper_level = priorities.front().second;
  const auto& upper_ssts = snapshot.levels[upper_level - 1].sst_ids;
  if (upper_ssts.empty() || upper_level >= options_.max_levels) {
    throw std::logic_error("invalid leveled compaction priority");
  }
  const SstId selected = *std::min_element(upper_ssts.begin(), upper_ssts.end());
  return LeveledCompactionTask{
      .upper_level = upper_level,
      .upper_level_sst_ids = {selected},
      .lower_level = upper_level + 1,
      .lower_level_sst_ids =
          find_overlapping_ssts(snapshot, {selected}, upper_level + 1),
      .is_lower_level_bottom_level =
          upper_level + 1 == options_.max_levels,
  };
}

ApplyResult LeveledCompactionController::apply_compaction_result(
    const CompactionState& snapshot, const LeveledCompactionTask& task,
    const std::vector<SstId>& output, bool in_recovery) const {
  if (task.lower_level == 0 ||
      task.lower_level > snapshot.levels.size()) {
    throw std::invalid_argument("invalid lower level in compaction task");
  }

  ApplyResult result{.state = snapshot, .files_to_remove = {}};
  std::unordered_set<SstId> upper_to_remove(
      task.upper_level_sst_ids.begin(), task.upper_level_sst_ids.end());
  std::unordered_set<SstId> lower_to_remove(
      task.lower_level_sst_ids.begin(), task.lower_level_sst_ids.end());

  auto remove_selected = [](std::vector<SstId>& ids,
                            std::unordered_set<SstId>& selected) {
    std::vector<SstId> retained;
    retained.reserve(ids.size());
    for (SstId id : ids) {
      if (selected.erase(id) == 0) {
        retained.push_back(id);
      }
    }
    if (!selected.empty()) {
      throw std::invalid_argument("a task SST is no longer present");
    }
    ids = std::move(retained);
  };

  if (task.upper_level) {
    if (*task.upper_level == 0 ||
        *task.upper_level > result.state.levels.size()) {
      throw std::invalid_argument("invalid upper level in compaction task");
    }
    remove_selected(result.state.levels[*task.upper_level - 1].sst_ids,
                    upper_to_remove);
  } else {
    remove_selected(result.state.l0_sstables, upper_to_remove);
  }

  result.files_to_remove.insert(result.files_to_remove.end(),
                                task.upper_level_sst_ids.begin(),
                                task.upper_level_sst_ids.end());
  result.files_to_remove.insert(result.files_to_remove.end(),
                                task.lower_level_sst_ids.begin(),
                                task.lower_level_sst_ids.end());

  auto& lower = result.state.levels[task.lower_level - 1].sst_ids;
  remove_selected(lower, lower_to_remove);
  lower.insert(lower.end(), output.begin(), output.end());
  if (!in_recovery) {
    std::sort(lower.begin(), lower.end(), [&](SstId lhs, SstId rhs) {
      return metadata(result.state, lhs).first_key <
             metadata(result.state, rhs).first_key;
    });
  }
  return result;
}

}  // namespace hermesdb
