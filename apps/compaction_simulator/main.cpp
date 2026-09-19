// Educational compaction-policy simulator for hermesdb.
//
// This models file placement and amplification, not SST contents. It is useful
// for comparing policy behavior without creating a database.

#if __has_include("hermesdb/compaction.hpp")
#include "hermesdb/compaction.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::string policy{"all"};
  std::size_t iterations{24};
  std::size_t l0_trigger{4};
  std::size_t max_levels{4};
  std::size_t tier_width{4};
  std::size_t level_multiplier{4};
  std::uint64_t seed{42};
  bool quiet{false};
};

struct File {
  std::uint64_t id{};
  std::uint64_t origin{};
  std::uint64_t size{1};
  std::uint64_t first{};
  std::uint64_t last{};
};

struct Metrics {
  std::uint64_t flushed_bytes{};
  std::uint64_t compacted_bytes{};
  std::uint64_t compactions{};
  double peak_space_amplification{1.0};
};

struct Simulation {
  std::vector<File> l0;
  std::vector<std::vector<File>> levels;
  Metrics metrics;
  std::uint64_t next_id{1};
};

[[noreturn]] void usage(const char* program, std::string_view error = {}) {
  if (!error.empty()) {
    std::cerr << "error: " << error << "\n\n";
  }
  std::cerr
      << "Usage: " << program << " [options]\n"
      << "  --policy simple|leveled|tiered|all  Policy to demonstrate (default: all)\n"
      << "  --iterations N                    Number of flushes (default: 24)\n"
      << "  --l0-trigger N                    L0 files before compaction (default: 4)\n"
      << "  --max-levels N                    Number of disk levels (default: 4)\n"
      << "  --tier-width N                    Tiers merged together (default: 4)\n"
      << "  --level-multiplier N              Per-level capacity ratio (default: 4)\n"
      << "  --seed N                          Key-range RNG seed (default: 42)\n"
      << "  --quiet                           Print only final metrics\n"
      << "  --help                            Show this help\n";
  std::exit(error.empty() ? 0 : 2);
}

std::size_t number(std::string_view text, std::string_view flag) {
  std::size_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0) {
    throw std::runtime_error(std::string(flag) + " expects a positive integer");
  }
  return value;
}

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
    }
    if (arg == "--quiet") {
      options.quiet = true;
      continue;
    }
    if (i + 1 >= argc) {
      usage(argv[0], std::string(arg) + " requires a value");
    }
    const std::string_view value(argv[++i]);
    if (arg == "--policy") {
      options.policy = value;
    } else if (arg == "--iterations") {
      options.iterations = number(value, arg);
    } else if (arg == "--l0-trigger") {
      options.l0_trigger = number(value, arg);
    } else if (arg == "--max-levels") {
      options.max_levels = number(value, arg);
    } else if (arg == "--tier-width") {
      options.tier_width = number(value, arg);
    } else if (arg == "--level-multiplier") {
      options.level_multiplier = number(value, arg);
    } else if (arg == "--seed") {
      options.seed = number(value, arg);
    } else {
      usage(argv[0], std::string("unknown option: ") + std::string(arg));
    }
  }
  if (options.policy != "all" && options.policy != "simple" &&
      options.policy != "leveled" && options.policy != "tiered") {
    usage(argv[0], "--policy must be simple, leveled, tiered, or all");
  }
  return options;
}

File merge_files(Simulation& simulation, const std::vector<File>& input) {
  File output;
  output.id = simulation.next_id++;
  output.origin = input.front().origin;
  output.first = input.front().first;
  output.last = input.front().last;
  for (const auto& file : input) {
    output.size += file.size;
    output.first = std::min(output.first, file.first);
    output.last = std::max(output.last, file.last);
    simulation.metrics.compacted_bytes += file.size;
  }
  --output.size;  // File starts with one unit for convenient aggregate init.
  simulation.metrics.peak_space_amplification =
      std::max(simulation.metrics.peak_space_amplification,
               1.0 + static_cast<double>(output.size) /
                         static_cast<double>(simulation.metrics.flushed_bytes));
  ++simulation.metrics.compactions;
  return output;
}

void dump(const Simulation& simulation, std::size_t iteration) {
  std::cout << "flush " << std::setw(3) << iteration << " | L0:";
  for (const auto& file : simulation.l0) {
    std::cout << ' ' << file.id;
  }
  for (std::size_t i = 0; i < simulation.levels.size(); ++i) {
    std::cout << " | L" << i + 1 << ':';
    for (const auto& file : simulation.levels[i]) {
      std::cout << ' ' << file.id << '(' << file.size << ')';
    }
  }
  std::cout << '\n';
}

void compact_simple(Simulation& simulation, const Options& options) {
  if (simulation.l0.size() < options.l0_trigger) {
    return;
  }
  std::vector<File> input = std::move(simulation.l0);
  simulation.l0.clear();
  input.insert(input.end(), simulation.levels[0].begin(),
               simulation.levels[0].end());
  simulation.levels[0] = {merge_files(simulation, input)};

  for (std::size_t level = 0; level + 1 < simulation.levels.size(); ++level) {
    const std::uint64_t upper_size =
        simulation.levels[level].empty() ? 0 : simulation.levels[level][0].size;
    const std::uint64_t lower_size = simulation.levels[level + 1].empty()
                                         ? 0
                                         : simulation.levels[level + 1][0].size;
    if (upper_size <= lower_size * options.level_multiplier) {
      break;
    }
    input = std::move(simulation.levels[level]);
    simulation.levels[level].clear();
    input.insert(input.end(), simulation.levels[level + 1].begin(),
                 simulation.levels[level + 1].end());
    simulation.levels[level + 1] = {merge_files(simulation, input)};
  }
}

void compact_leveled(Simulation& simulation, const Options& options) {
  if (simulation.l0.size() >= options.l0_trigger) {
    std::vector<File> input = std::move(simulation.l0);
    simulation.l0.clear();
    input.insert(input.end(), simulation.levels[0].begin(),
                 simulation.levels[0].end());
    simulation.levels[0] = {merge_files(simulation, input)};
  }
  std::uint64_t capacity = options.l0_trigger;
  for (std::size_t level = 0; level + 1 < simulation.levels.size(); ++level) {
    std::uint64_t size{};
    for (const auto& file : simulation.levels[level]) {
      size += file.size;
    }
    if (size <= capacity) {
      break;
    }
    std::vector<File> input = std::move(simulation.levels[level]);
    simulation.levels[level].clear();
    input.insert(input.end(), simulation.levels[level + 1].begin(),
                 simulation.levels[level + 1].end());
    simulation.levels[level + 1] = {merge_files(simulation, input)};
    capacity *= options.level_multiplier;
  }
}

void compact_tiered(Simulation& simulation, const Options& options) {
  if (simulation.levels.size() < options.tier_width) {
    return;
  }
  std::vector<File> input;
  for (std::size_t i = 0; i < options.tier_width; ++i) {
    input.insert(input.end(), simulation.levels[i].begin(),
                 simulation.levels[i].end());
  }
  auto output = merge_files(simulation, input);
  simulation.levels.erase(simulation.levels.begin(),
                          simulation.levels.begin() +
                              static_cast<std::ptrdiff_t>(options.tier_width));
  simulation.levels.insert(simulation.levels.begin(), {output});
}

void print_metrics(std::string_view policy, const Simulation& simulation) {
  const double write_amp =
      static_cast<double>(simulation.metrics.flushed_bytes +
                          simulation.metrics.compacted_bytes) /
      static_cast<double>(simulation.metrics.flushed_bytes);
  std::size_t read_amp = simulation.l0.size();
  read_amp += static_cast<std::size_t>(std::count_if(
      simulation.levels.begin(), simulation.levels.end(),
      [](const auto& level) { return !level.empty(); }));
  std::cout << std::fixed << std::setprecision(2) << policy
            << ": flushes=" << simulation.metrics.flushed_bytes
            << " compactions=" << simulation.metrics.compactions
            << " write_amp=" << write_amp << "x"
            << " peak_space=" << simulation.metrics.peak_space_amplification
            << "x"
            << " read_amp~" << read_amp << " runs\n";
}

void run(std::string_view policy, const Options& options) {
  Simulation simulation;
  if (policy != "tiered") {
    simulation.levels.resize(options.max_levels);
  }
  std::mt19937_64 random(options.seed);
  std::uniform_int_distribution<std::uint64_t> begin_distribution(0, 1'000'000);
  std::uniform_int_distribution<std::uint64_t> width_distribution(1'000, 100'000);

  if (!options.quiet) {
    std::cout << "\n== " << policy << " compaction ==\n";
  }
  for (std::size_t iteration = 1; iteration <= options.iterations; ++iteration) {
    const auto first = begin_distribution(random);
    File file{simulation.next_id++, simulation.next_id - 1, 1, first,
              first + width_distribution(random)};
    ++simulation.metrics.flushed_bytes;
    if (policy == "tiered") {
      simulation.levels.insert(simulation.levels.begin(), {file});
      compact_tiered(simulation, options);
    } else {
      simulation.l0.push_back(file);
      if (policy == "simple") {
        compact_simple(simulation, options);
      } else {
        compact_leveled(simulation, options);
      }
    }
    if (!options.quiet) {
      dump(simulation, iteration);
    }
  }
  print_metrics(policy, simulation);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_args(argc, argv);
    if (options.policy == "all") {
      run("simple", options);
      run("leveled", options);
      run("tiered", options);
    } else {
      run(options.policy, options);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
