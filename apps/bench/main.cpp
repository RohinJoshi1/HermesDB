#include "bench.hpp"

#include "hermesdb/db.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif !defined(_WIN32)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace {

using hermesdb::bench::Distribution;
using hermesdb::bench::Histogram;
using hermesdb::bench::KeyChooser;
using hermesdb::bench::make_key;
using hermesdb::bench::make_value;
using hermesdb::bench::now;

struct Config {
  std::string workload{"fillrandom"};
  std::filesystem::path path;
  std::uint64_t num{10000};
  std::uint64_t ops{0};
  std::size_t value_size{100};
  unsigned threads{1};
  unsigned seconds{0};
  unsigned scan_len{10};
  Distribution distribution{Distribution::zipfian};
  bool enable_wal{true};
  std::string compaction{"leveled"};
  std::size_t target_sst_size{1U << 20U};
  std::size_t block_size{4096};
  std::string compression{"none"};
  bool json{false};
  bool keep_db{false};
  std::uint64_t seed{1};
};

[[noreturn]] void usage(const char* program, std::string_view error = {}) {
  if (!error.empty()) std::cerr << "error: " << error << "\n\n";
  std::cerr
      << "Usage: " << program << " [options]\n"
      << "  --workload NAME     fillseq|fillrandom|overwrite|readrandom|\n"
      << "                      readseq|readwhilewriting|scan|deleterandom|\n"
      << "                      ycsb-a|ycsb-b|ycsb-c|ycsb-d|ycsb-e|ycsb-f\n"
      << "  --num N             keys to load (default 10000)\n"
      << "  --ops N             run-phase operations (default = --num)\n"
      << "  --value-size N      value bytes (default 100)\n"
      << "  --threads N         worker threads, 0 = hardware concurrency\n"
      << "  --seconds N         stop the run phase after N seconds\n"
      << "  --scan-len N        keys per scan (default 10)\n"
      << "  --distribution D    uniform|zipfian|latest (YCSB default zipfian)\n"
      << "  --wal / --no-wal    WAL on by default\n"
      << "  --compaction P      simple|leveled|tiered|none\n"
      << "  --sst-size N        target_sst_size bytes\n"
      << "  --block-size N      SST block size\n"
      << "  --compression C     none|zlib (default none)\n"
      << "  --path DIR          database directory (default: temp)\n"
      << "  --seed N            RNG seed\n"
      << "  --json              machine-readable output\n"
      << "  --keep-db           do not delete --path after the run\n";
  std::exit(error.empty() ? 0 : 2);
}

std::uint64_t parse_u64(std::string_view text, std::string_view flag) {
  try {
    return std::stoull(std::string(text));
  } catch (...) {
    usage("hermesdb_bench", std::string(flag) + " expects an integer");
  }
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    auto need = [&](std::string_view flag) -> std::string_view {
      if (++i >= argc) usage(argv[0], std::string(flag) + " requires a value");
      return argv[i];
    };
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
    } else if (arg == "--workload") {
      config.workload = std::string(need(arg));
    } else if (arg == "--num") {
      config.num = parse_u64(need(arg), arg);
    } else if (arg == "--ops") {
      config.ops = parse_u64(need(arg), arg);
    } else if (arg == "--value-size") {
      config.value_size = static_cast<std::size_t>(parse_u64(need(arg), arg));
    } else if (arg == "--threads") {
      config.threads = static_cast<unsigned>(parse_u64(need(arg), arg));
    } else if (arg == "--seconds") {
      config.seconds = static_cast<unsigned>(parse_u64(need(arg), arg));
    } else if (arg == "--scan-len") {
      config.scan_len = static_cast<unsigned>(parse_u64(need(arg), arg));
    } else if (arg == "--distribution") {
      const auto value = need(arg);
      if (value == "uniform") config.distribution = Distribution::uniform;
      else if (value == "zipfian") config.distribution = Distribution::zipfian;
      else if (value == "latest") config.distribution = Distribution::latest;
      else usage(argv[0], "unknown distribution");
    } else if (arg == "--wal") {
      config.enable_wal = true;
    } else if (arg == "--no-wal") {
      config.enable_wal = false;
    } else if (arg == "--compaction") {
      config.compaction = std::string(need(arg));
    } else if (arg == "--sst-size") {
      config.target_sst_size =
          static_cast<std::size_t>(parse_u64(need(arg), arg));
    } else if (arg == "--block-size") {
      config.block_size = static_cast<std::size_t>(parse_u64(need(arg), arg));
    } else if (arg == "--compression") {
      config.compression = std::string(need(arg));
      if (config.compression != "none" && config.compression != "zlib") {
        usage(argv[0], "--compression must be none or zlib");
      }
    } else if (arg == "--path") {
      config.path = need(arg);
    } else if (arg == "--seed") {
      config.seed = parse_u64(need(arg), arg);
    } else if (arg == "--json") {
      config.json = true;
    } else if (arg == "--keep-db") {
      config.keep_db = true;
    } else {
      usage(argv[0], "unknown option: " + std::string(arg));
    }
  }
  if (config.ops == 0) config.ops = config.num;
  if (config.threads == 0) {
    config.threads = std::max(1u, std::thread::hardware_concurrency());
  }
  if (config.path.empty()) {
    config.path = std::filesystem::temp_directory_path() /
                  ("hermesdb-bench-" + std::to_string(::getpid()));
  }
  return config;
}

hermesdb::CompactionOptions compaction_options(std::string_view policy) {
  if (policy == "simple") {
    return hermesdb::SimpleLeveledCompactionOptions{};
  }
  if (policy == "tiered") return hermesdb::TieredCompactionOptions{};
  if (policy == "none") return hermesdb::NoCompactionOptions{};
  if (policy != "leveled") {
    usage("hermesdb_bench", "--compaction must be simple, leveled, tiered, or none");
  }
  return hermesdb::LeveledCompactionOptions{};
}

std::string shell(const char* command) {
  std::string output;
#ifdef _WIN32
  static_cast<void>(command);
#else
  FILE* pipe = ::popen(command, "r");
  if (pipe == nullptr) return {};
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  static_cast<void>(::pclose(pipe));
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
#endif
  return output;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::uint64_t directory_bytes(const std::filesystem::path& path) {
  std::uint64_t total = 0;
  std::error_code ignored;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(path, ignored)) {
    if (entry.is_regular_file(ignored)) total += entry.file_size(ignored);
  }
  return total;
}

std::string cpu_model() {
#ifdef __APPLE__
  char buffer[256];
  std::size_t size = sizeof(buffer);
  if (::sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) ==
      0) {
    return {buffer, size - (size > 0 ? 1 : 0)};
  }
#endif
  return shell("sysctl -n machdep.cpu.brand_string 2>/dev/null");
}

std::string memory_bytes() {
#ifdef __APPLE__
  std::uint64_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname("hw.memsize", &value, &size, nullptr, 0) == 0) {
    return std::to_string(value);
  }
#endif
  return {};
}

std::string uname_string() {
#ifndef _WIN32
  struct utsname info {};
  if (::uname(&info) == 0) {
    return std::string(info.sysname) + " " + info.release + " " + info.machine;
  }
#endif
  return {};
}

struct Mix {
  int read{0};
  int update{0};
  int insert{0};
  int scan{0};
  int rmw{0};
  int erase{0};
};

Mix mix_for(std::string_view workload) {
  if (workload == "ycsb-a") return {.read = 50, .update = 50};
  if (workload == "ycsb-b") return {.read = 95, .update = 5};
  if (workload == "ycsb-c") return {.read = 100};
  if (workload == "ycsb-d") return {.read = 95, .insert = 5};
  if (workload == "ycsb-e") return {.insert = 5, .scan = 95};
  if (workload == "ycsb-f") return {.read = 50, .rmw = 50};
  if (workload == "deleterandom") return {.erase = 100};
  if (workload == "scan") return {.scan = 100};
  if (workload == "readrandom" || workload == "readseq") return {.read = 100};
  if (workload == "readwhilewriting") return {.read = 90, .update = 10};
  if (workload == "overwrite") return {.update = 100};
  return {};
}

bool is_fill(std::string_view workload) {
  return workload == "fillseq" || workload == "fillrandom";
}

bool needs_load(std::string_view workload) { return !is_fill(workload); }

void put_id(hermesdb::DB& db, std::uint64_t id, std::size_t value_size) {
  db.Put(make_key(id), make_value(id, value_size));
}

void load_keys(hermesdb::DB& db, const Config& config, bool sequential) {
  const auto start = now();
  for (std::uint64_t i = 0; i < config.num; ++i) {
    const auto id = sequential ? i : hermesdb::bench::scramble(i) % config.num;
    put_id(db, id, config.value_size);
  }
  const auto elapsed = now() - start;
  if (!config.json) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cerr << "loaded " << config.num << " keys in " << ms << " ms\n";
  }
}

struct RunResult {
  Histogram latency;
  std::uint64_t ops{};
  std::uint64_t elapsed_ns{};
  std::uint64_t found{};
};

RunResult run_workers(hermesdb::DB& db, const Config& config,
                      const Mix& mix, bool sequential_fill) {
  std::atomic<std::uint64_t> next_insert{config.num};
  std::atomic<bool> stop{false};
  std::mutex histogram_mutex;
  Histogram combined;
  std::atomic<std::uint64_t> found{0};
  std::atomic<std::uint64_t> completed{0};

  const auto deadline =
      config.seconds == 0
          ? std::chrono::steady_clock::time_point::max()
          : now() + std::chrono::seconds(config.seconds);
  const auto ops_target = config.seconds == 0 ? config.ops : ~std::uint64_t{0};

  const auto started = now();
  std::vector<std::thread> workers;
  workers.reserve(config.threads);
  for (unsigned t = 0; t < config.threads; ++t) {
    workers.emplace_back([&, t] {
      Histogram local;
      std::uint64_t local_found = 0;
      KeyChooser chooser(config.distribution, config.num,
                         config.seed + 1000U * t);
      std::mt19937_64 coin(config.seed + 17U * t + 1);
      std::uniform_int_distribution<int> percent(0, 99);
      const auto chunk = std::max<std::uint64_t>(
          1, (config.num + config.threads - 1) / config.threads);
      const auto begin = static_cast<std::uint64_t>(t) * chunk;
      const auto end = std::min(config.num, begin + chunk);
      std::uint64_t seq = begin;

      while (!stop.load(std::memory_order_relaxed) &&
             completed.load(std::memory_order_relaxed) < ops_target &&
             now() < deadline) {
        const auto op_start = now();
        if (config.workload == "fillseq" || sequential_fill) {
          if (seq >= end) break;
          put_id(db, seq++, config.value_size);
        } else if (config.workload == "fillrandom") {
          if (seq >= end) break;
          put_id(db, hermesdb::bench::scramble(seq++) % config.num,
                 config.value_size);
        } else if (config.workload == "readseq") {
          if (seq >= end) seq = begin;
          const auto got = db.Get(make_key(seq++));
          if (got) ++local_found;
        } else {
          const int roll = percent(coin);
          int cursor = 0;
          auto take = [&](int share) {
            cursor += share;
            return roll < cursor;
          };
          if (take(mix.read)) {
            const auto got = db.Get(make_key(chooser.next()));
            if (got) ++local_found;
          } else if (take(mix.update)) {
            const auto id = chooser.next();
            put_id(db, id, config.value_size);
          } else if (take(mix.insert)) {
            put_id(db, next_insert.fetch_add(1, std::memory_order_relaxed),
                   config.value_size);
          } else if (take(mix.scan)) {
            const auto start_id = chooser.next();
            auto iterator =
                db.Scan(hermesdb::KeyBound::Included(make_key(start_id)),
                        hermesdb::KeyBound::Unbounded());
            std::uint64_t seen = 0;
            while (iterator.valid() && seen < config.scan_len) {
              ++seen;
              iterator.next();
            }
          } else if (take(mix.rmw)) {
            const auto id = chooser.next();
            static_cast<void>(db.Get(make_key(id)));
            put_id(db, id, config.value_size);
          } else if (take(mix.erase)) {
            db.Delete(make_key(chooser.next()));
          } else {
            put_id(db, chooser.next(), config.value_size);
          }
        }
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now() -
                                                                op_start)
                .count());
        local.add(ns);
        completed.fetch_add(1, std::memory_order_relaxed);
      }
      found.fetch_add(local_found, std::memory_order_relaxed);
      std::lock_guard lock(histogram_mutex);
      combined.merge(local);
    });
  }
  for (auto& worker : workers) worker.join();
  stop.store(true, std::memory_order_relaxed);
  const auto elapsed = now() - started;
  RunResult result;
  result.latency = combined;
  result.ops = completed.load();
  result.elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  result.found = found.load();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  auto config = parse_args(argc, argv);
  std::filesystem::remove_all(config.path);
  std::filesystem::create_directories(config.path);

  hermesdb::Options options;
  options.block_size = config.block_size;
  options.target_sst_size = config.target_sst_size;
  options.enable_wal = config.enable_wal;
  options.compaction_options = compaction_options(config.compaction);
  options.compression = config.compression == "zlib"
                            ? hermesdb::Compression::zlib
                            : hermesdb::Compression::none;

  auto db = hermesdb::DB::Open(config.path, options);
  if (needs_load(config.workload)) {
    load_keys(*db, config, false);
  }

  const auto mix = mix_for(config.workload);
  const bool sequential_fill = config.workload == "fillseq";
  const auto result =
      run_workers(*db, config, mix, sequential_fill);
  db->Sync();
  const auto metrics = db->Metrics();
  const auto live_bytes = directory_bytes(config.path);
  db->Close();

  const double seconds = static_cast<double>(result.elapsed_ns) / 1e9;
  const double ops_per_sec =
      seconds <= 0.0 ? 0.0 : static_cast<double>(result.ops) / seconds;
  const double write_amp =
      metrics.write_user_bytes == 0
          ? 0.0
          : static_cast<double>(metrics.wal_bytes + metrics.flush_bytes +
                                metrics.compaction_output_bytes) /
                static_cast<double>(metrics.write_user_bytes);
  const double space_amp =
      metrics.write_user_bytes == 0
          ? 0.0
          : static_cast<double>(live_bytes) /
                static_cast<double>(metrics.write_user_bytes);
  const double block_compression =
      metrics.sst_raw_bytes == 0
          ? 1.0
          : static_cast<double>(metrics.sst_stored_bytes) /
                static_cast<double>(metrics.sst_raw_bytes);

  const auto commit = shell("git rev-parse --short HEAD 2>/dev/null");
  const auto compiler =
#ifdef __clang__
      std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
      std::string("gcc ") + __VERSION__;
#else
      std::string("unknown");
#endif

  if (config.json) {
    std::cout << "{\n"
              << "  \"workload\": \"" << json_escape(config.workload) << "\",\n"
              << "  \"ops\": " << result.ops << ",\n"
              << "  \"elapsed_ns\": " << result.elapsed_ns << ",\n"
              << "  \"ops_per_sec\": " << ops_per_sec << ",\n"
              << "  \"found\": " << result.found << ",\n"
              << "  \"latency_ns\": {\n"
              << "    \"avg\": " << result.latency.avg_ns() << ",\n"
              << "    \"p50\": " << result.latency.percentile(0.50) << ",\n"
              << "    \"p95\": " << result.latency.percentile(0.95) << ",\n"
              << "    \"p99\": " << result.latency.percentile(0.99) << ",\n"
              << "    \"p99_9\": " << result.latency.percentile(0.999) << ",\n"
              << "    \"max\": " << result.latency.max_ns() << "\n"
              << "  },\n"
              << "  \"engine\": {\n"
              << "    \"write_ops\": " << metrics.write_ops << ",\n"
              << "    \"write_user_bytes\": " << metrics.write_user_bytes << ",\n"
              << "    \"wal_bytes\": " << metrics.wal_bytes << ",\n"
              << "    \"get_ops\": " << metrics.get_ops << ",\n"
              << "    \"get_hits\": " << metrics.get_hits << ",\n"
              << "    \"scan_ops\": " << metrics.scan_ops << ",\n"
              << "    \"scan_keys\": " << metrics.scan_keys << ",\n"
              << "    \"freeze_count\": " << metrics.freeze_count << ",\n"
              << "    \"flush_count\": " << metrics.flush_count << ",\n"
              << "    \"flush_bytes\": " << metrics.flush_bytes << ",\n"
              << "    \"compaction_count\": " << metrics.compaction_count << ",\n"
              << "    \"compaction_input_bytes\": "
              << metrics.compaction_input_bytes << ",\n"
              << "    \"compaction_output_bytes\": "
              << metrics.compaction_output_bytes << ",\n"
              << "    \"sst_raw_bytes\": " << metrics.sst_raw_bytes << ",\n"
              << "    \"sst_stored_bytes\": " << metrics.sst_stored_bytes
              << ",\n"
              << "    \"immutable_memtables\": " << metrics.immutable_memtables
              << ",\n"
              << "    \"l0_tables\": " << metrics.l0_tables << ",\n"
              << "    \"l1_tables\": " << metrics.l1_tables << ",\n"
              << "    \"block_cache_hits\": " << metrics.block_cache_hits
              << ",\n"
              << "    \"block_cache_misses\": " << metrics.block_cache_misses
              << ",\n"
              << "    \"live_bytes\": " << live_bytes << "\n"
              << "  },\n"
              << "  \"amplification\": {\n"
              << "    \"write\": " << write_amp << ",\n"
              << "    \"space\": " << space_amp << ",\n"
              << "    \"block_compression\": " << block_compression << ",\n"
              << "    \"read\": null,\n"
              << "    \"block_cache_hits\": " << metrics.block_cache_hits
              << ",\n"
              << "    \"block_cache_misses\": " << metrics.block_cache_misses
              << "\n"
              << "  },\n"
              << "  \"environment\": {\n"
              << "    \"commit\": \"" << json_escape(commit) << "\",\n"
              << "    \"compiler\": \"" << json_escape(compiler) << "\",\n"
              << "    \"uname\": \"" << json_escape(uname_string()) << "\",\n"
              << "    \"cpu\": \"" << json_escape(cpu_model()) << "\",\n"
              << "    \"memory_bytes\": \"" << json_escape(memory_bytes())
              << "\",\n"
              << "    \"hardware_concurrency\": "
              << std::thread::hardware_concurrency() << ",\n"
              << "    \"threads\": " << config.threads << ",\n"
              << "    \"wal\": " << (config.enable_wal ? "true" : "false")
              << ",\n"
              << "    \"compaction\": \"" << json_escape(config.compaction)
              << "\",\n"
              << "    \"compression\": \"" << json_escape(config.compression)
              << "\",\n"
              << "    \"num\": " << config.num << ",\n"
              << "    \"value_size\": " << config.value_size << ",\n"
              << "    \"seed\": " << config.seed << "\n"
              << "  },\n"
              << "  \"notes\": [\n"
              << "    \"block cache is an LRU of decoded SST data blocks\",\n"
              << "    \"io_uring is not implemented\",\n"
              << "    \"scans are lazy heap-merged cursors\"\n"
              << "  ]\n"
              << "}\n";
  } else {
    std::cout << "workload          " << config.workload << '\n'
              << "ops               " << result.ops << '\n'
              << "ops/sec           " << ops_per_sec << '\n'
              << "p50 ns            " << result.latency.percentile(0.50) << '\n'
              << "p95 ns            " << result.latency.percentile(0.95) << '\n'
              << "p99 ns            " << result.latency.percentile(0.99) << '\n'
              << "p99.9 ns          " << result.latency.percentile(0.999) << '\n'
              << "write_amp         " << write_amp << '\n'
              << "space_amp         " << space_amp << '\n'
              << "block_compression " << block_compression << '\n'
              << "flush_count       " << metrics.flush_count << '\n'
              << "compaction_count  " << metrics.compaction_count << '\n'
              << "l0_tables         " << metrics.l0_tables << '\n'
              << "l1_tables         " << metrics.l1_tables << '\n'
              << "cache_hits        " << metrics.block_cache_hits << '\n'
              << "cache_misses      " << metrics.block_cache_misses << '\n';
  }

  if (!config.keep_db) std::filesystem::remove_all(config.path);
  return 0;
}
