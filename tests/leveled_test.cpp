#include "hermesdb/compaction.hpp"

#include <cassert>
#include <iostream>

using namespace hermesdb;

int main() {
  CompactionState state{
      .l0_sstables = {5, 4},
      .levels = {{1, {}}, {2, {}}, {3, {30, 31}}},
      .sstables = {{4, {"b", "d", 100}},
                   {5, {"f", "h", 100}},
                   {30, {"a", "e", 1U << 20U}},
                   {31, {"f", "z", 1U << 20U}},
                   {40, {"a", "e", 100}},
                   {41, {"f", "z", 100}}}};
  LeveledCompactionController controller(LeveledCompactionOptions{
      .level_size_multiplier = 10,
      .level0_file_num_compaction_trigger = 2,
      .max_levels = 3,
      .base_level_size_mb = 1});
  const auto task = controller.generate_compaction_task(state);
  assert(task);
  assert(!task->upper_level);
  assert(task->lower_level == 2);
  assert(task->lower_level_sst_ids.empty());
  assert(!task->is_lower_level_bottom_level);

  state.l0_sstables.insert(state.l0_sstables.begin(), 6);
  const auto result =
      controller.apply_compaction_result(state, *task, {41, 40});
  assert((result.state.l0_sstables == std::vector<SstId>{6}));
  assert((result.state.levels[1].sst_ids == std::vector<SstId>{40, 41}));
  assert((result.files_to_remove == std::vector<SstId>{5, 4}));
  std::cout << "Leveled compaction tests passed\n";
}
