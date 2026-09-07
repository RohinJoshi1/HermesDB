#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace tiny_lsm {

using SstId = std::uint64_t;

struct SstMetadata {
  std::string first_key;
  std::string last_key;
  std::size_t table_size = 0;

  friend bool operator==(const SstMetadata&, const SstMetadata&) = default;
};

struct Level {
  SstId id = 0;
  std::vector<SstId> sst_ids;

  friend bool operator==(const Level&, const Level&) = default;
};

// The policy layer only needs the physical layout and a small metadata view.
// Other storage-engine state can remain outside this snapshot.
struct CompactionState {
  std::vector<SstId> l0_sstables;
  std::vector<Level> levels;
  std::unordered_map<SstId, SstMetadata> sstables;

  friend bool operator==(const CompactionState&, const CompactionState&) = default;
};

struct ApplyResult {
  CompactionState state;
  std::vector<SstId> files_to_remove;
};

struct NoCompactionOptions {
  friend bool operator==(const NoCompactionOptions&, const NoCompactionOptions&) = default;
};

struct SimpleLeveledCompactionOptions {
  std::size_t size_ratio_percent = 200;
  std::size_t level0_file_num_compaction_trigger = 4;
  std::size_t max_levels = 3;

  friend bool operator==(const SimpleLeveledCompactionOptions&,
                         const SimpleLeveledCompactionOptions&) = default;
};

struct SimpleLeveledCompactionTask {
  // nullopt denotes L0.
  std::optional<std::size_t> upper_level;
  std::vector<SstId> upper_level_sst_ids;
  std::size_t lower_level = 0;
  std::vector<SstId> lower_level_sst_ids;
  bool is_lower_level_bottom_level = false;

  friend bool operator==(const SimpleLeveledCompactionTask&,
                         const SimpleLeveledCompactionTask&) = default;
};

class SimpleLeveledCompactionController {
 public:
  explicit SimpleLeveledCompactionController(SimpleLeveledCompactionOptions options);

  [[nodiscard]] std::optional<SimpleLeveledCompactionTask>
  generate_compaction_task(const CompactionState& snapshot) const;
  [[nodiscard]] ApplyResult apply_compaction_result(
      const CompactionState& snapshot, const SimpleLeveledCompactionTask& task,
      const std::vector<SstId>& output) const;
  [[nodiscard]] const SimpleLeveledCompactionOptions& options() const noexcept {
    return options_;
  }

 private:
  SimpleLeveledCompactionOptions options_;
};

struct LeveledCompactionOptions {
  std::size_t level_size_multiplier = 10;
  std::size_t level0_file_num_compaction_trigger = 4;
  std::size_t max_levels = 7;
  std::size_t base_level_size_mb = 64;

  friend bool operator==(const LeveledCompactionOptions&,
                         const LeveledCompactionOptions&) = default;
};

struct LeveledCompactionTask {
  // nullopt denotes L0.
  std::optional<std::size_t> upper_level;
  std::vector<SstId> upper_level_sst_ids;
  std::size_t lower_level = 0;
  std::vector<SstId> lower_level_sst_ids;
  bool is_lower_level_bottom_level = false;

  friend bool operator==(const LeveledCompactionTask&,
                         const LeveledCompactionTask&) = default;
};

class LeveledCompactionController {
 public:
  explicit LeveledCompactionController(LeveledCompactionOptions options);

  [[nodiscard]] std::optional<LeveledCompactionTask>
  generate_compaction_task(const CompactionState& snapshot) const;
  [[nodiscard]] ApplyResult apply_compaction_result(
      const CompactionState& snapshot, const LeveledCompactionTask& task,
      const std::vector<SstId>& output, bool in_recovery = false) const;
  [[nodiscard]] const LeveledCompactionOptions& options() const noexcept {
    return options_;
  }

 private:
  [[nodiscard]] std::vector<SstId> find_overlapping_ssts(
      const CompactionState& snapshot, const std::vector<SstId>& sst_ids,
      std::size_t in_level) const;

  LeveledCompactionOptions options_;
};

struct TieredCompactionOptions {
  std::size_t num_tiers = 3;
  std::size_t max_size_amplification_percent = 200;
  std::size_t size_ratio = 1;
  std::size_t min_merge_width = 2;
  std::optional<std::size_t> max_merge_width;

  friend bool operator==(const TieredCompactionOptions&,
                         const TieredCompactionOptions&) = default;
};

struct TieredCompactionTask {
  std::vector<Level> tiers;
  bool bottom_tier_included = false;

  friend bool operator==(const TieredCompactionTask&,
                         const TieredCompactionTask&) = default;
};

class TieredCompactionController {
 public:
  explicit TieredCompactionController(TieredCompactionOptions options);

  [[nodiscard]] std::optional<TieredCompactionTask>
  generate_compaction_task(const CompactionState& snapshot) const;
  [[nodiscard]] ApplyResult apply_compaction_result(
      const CompactionState& snapshot, const TieredCompactionTask& task,
      const std::vector<SstId>& output) const;
  [[nodiscard]] const TieredCompactionOptions& options() const noexcept {
    return options_;
  }

 private:
  TieredCompactionOptions options_;
};

class NoCompactionController {
 public:
  explicit NoCompactionController(NoCompactionOptions = {}) {}

  [[nodiscard]] std::nullopt_t generate_compaction_task(
      const CompactionState&) const noexcept {
    return std::nullopt;
  }
};

struct ForceFullCompactionTask {
  std::vector<SstId> l0_sstables;
  std::vector<SstId> l1_sstables;

  friend bool operator==(const ForceFullCompactionTask&,
                         const ForceFullCompactionTask&) = default;
};

using CompactionOptions =
    std::variant<LeveledCompactionOptions, TieredCompactionOptions,
                 SimpleLeveledCompactionOptions, NoCompactionOptions>;
using CompactionTask =
    std::variant<LeveledCompactionTask, TieredCompactionTask,
                 SimpleLeveledCompactionTask, ForceFullCompactionTask>;

class CompactionController {
 public:
  explicit CompactionController(CompactionOptions options);

  [[nodiscard]] std::optional<CompactionTask> generate_compaction_task(
      const CompactionState& snapshot) const;
  [[nodiscard]] ApplyResult apply_compaction_result(
      const CompactionState& snapshot, const CompactionTask& task,
      const std::vector<SstId>& output, bool in_recovery = false) const;
  [[nodiscard]] bool flush_to_l0() const noexcept;

 private:
  using Controller =
      std::variant<LeveledCompactionController, TieredCompactionController,
                   SimpleLeveledCompactionController, NoCompactionController>;
  Controller controller_;
};

[[nodiscard]] bool compact_to_bottom_level(const CompactionTask& task) noexcept;

}  // namespace tiny_lsm
