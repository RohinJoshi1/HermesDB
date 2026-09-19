#include "hermesdb/compaction.hpp"

#include <cassert>
#include <iostream>

using namespace hermesdb;

int main() {
  CompactionState state{
      .levels = {{30, {30}}, {20, {20}}, {10, {10, 11, 12}}}};
  TieredCompactionController controller(TieredCompactionOptions{
      .num_tiers = 3,
      .max_size_amplification_percent = 1000,
      .size_ratio = 1,
      .min_merge_width = 2,
      .max_merge_width = 2});

  const auto task = controller.generate_compaction_task(state);
  assert(task);
  assert(task->tiers.size() == 2);
  assert(!task->bottom_tier_included);
  const auto result =
      controller.apply_compaction_result(state, *task, {40, 41});
  assert((result.state.levels[0] == Level{40, {40, 41}}));
  assert((result.state.levels[1] == Level{10, {10, 11, 12}}));
  assert((result.files_to_remove == std::vector<SstId>{30, 20}));

  CompactionController dispatch{TieredCompactionOptions{}};
  assert(!dispatch.flush_to_l0());
  std::cout << "Tiered compaction tests passed\n";
}
