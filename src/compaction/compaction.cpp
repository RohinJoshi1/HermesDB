#include "hermesdb/compaction.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hermesdb {

CompactionController::CompactionController(CompactionOptions options)
    : controller_(std::visit(
          [](auto&& value) -> Controller {
            using Options = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Options, LeveledCompactionOptions>) {
              return LeveledCompactionController(std::move(value));
            } else if constexpr (std::is_same_v<Options,
                                                TieredCompactionOptions>) {
              return TieredCompactionController(std::move(value));
            } else if constexpr (std::is_same_v<
                                     Options,
                                     SimpleLeveledCompactionOptions>) {
              return SimpleLeveledCompactionController(std::move(value));
            } else {
              return NoCompactionController(std::move(value));
            }
          },
          std::move(options))) {}

std::optional<CompactionTask> CompactionController::generate_compaction_task(
    const CompactionState& snapshot) const {
  return std::visit(
      [&snapshot](const auto& controller) -> std::optional<CompactionTask> {
        using ControllerType = std::decay_t<decltype(controller)>;
        if constexpr (std::is_same_v<ControllerType,
                                     NoCompactionController>) {
          return std::nullopt;
        } else {
          auto task = controller.generate_compaction_task(snapshot);
          if (!task) {
            return std::nullopt;
          }
          return CompactionTask(std::move(*task));
        }
      },
      controller_);
}

ApplyResult CompactionController::apply_compaction_result(
    const CompactionState& snapshot, const CompactionTask& task,
    const std::vector<SstId>& output, bool in_recovery) const {
  return std::visit(
      [&](const auto& controller) -> ApplyResult {
        using ControllerType = std::decay_t<decltype(controller)>;
        if constexpr (std::is_same_v<ControllerType,
                                     LeveledCompactionController>) {
          const auto* typed_task = std::get_if<LeveledCompactionTask>(&task);
          if (typed_task == nullptr) {
            throw std::invalid_argument("compaction controller/task mismatch");
          }
          return controller.apply_compaction_result(snapshot, *typed_task,
                                                     output, in_recovery);
        } else if constexpr (std::is_same_v<ControllerType,
                                            TieredCompactionController>) {
          const auto* typed_task = std::get_if<TieredCompactionTask>(&task);
          if (typed_task == nullptr) {
            throw std::invalid_argument("compaction controller/task mismatch");
          }
          return controller.apply_compaction_result(snapshot, *typed_task,
                                                     output);
        } else if constexpr (std::is_same_v<
                                 ControllerType,
                                 SimpleLeveledCompactionController>) {
          const auto* typed_task =
              std::get_if<SimpleLeveledCompactionTask>(&task);
          if (typed_task == nullptr) {
            throw std::invalid_argument("compaction controller/task mismatch");
          }
          return controller.apply_compaction_result(snapshot, *typed_task,
                                                     output);
        } else {
          throw std::logic_error(
              "no-compaction controller cannot apply a compaction result");
        }
      },
      controller_);
}

bool CompactionController::flush_to_l0() const noexcept {
  return !std::holds_alternative<TieredCompactionController>(controller_);
}

bool compact_to_bottom_level(const CompactionTask& task) noexcept {
  return std::visit(
      [](const auto& typed_task) {
        using Task = std::decay_t<decltype(typed_task)>;
        if constexpr (std::is_same_v<Task, LeveledCompactionTask> ||
                      std::is_same_v<Task, SimpleLeveledCompactionTask>) {
          return typed_task.is_lower_level_bottom_level;
        } else if constexpr (std::is_same_v<Task, TieredCompactionTask>) {
          return typed_task.bottom_tier_included;
        } else {
          return true;
        }
      },
      task);
}

}  // namespace hermesdb
