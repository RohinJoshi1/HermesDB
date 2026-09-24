#include "hermesdb/hermesdb.hpp"
#include "hermesdb/persistence.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

using namespace hermesdb;

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("hermesdb-wal-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(path);
  Options options;
  options.enable_wal = true;
  options.compaction_options = NoCompactionOptions{};
  {
    auto db = DB::Open(path, options);
    db->Put("a", "one");
    db->ForceFreezeMemTable();
    db->Put("b", "two");
    db->Delete("a");
    db->Sync();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    assert(!db->Get("a"));
    assert(as_string(*db->Get("b")) == "two");
    db->Put("c", "three");
    db->ForceFlush();
    db->Close();
  }
  {
    auto db = DB::Open(path, options);
    assert(as_string(*db->Get("b")) == "two");
    assert(as_string(*db->Get("c")) == "three");
    db->Close();
  }

  const auto concurrent_path = path / "concurrent.wal";
  {
    constexpr std::size_t kProducers = 8;
    constexpr std::uint64_t kRecords = 1000;
    auto wal = persistence::MvccWal::create(concurrent_path);
    std::atomic<bool> done{false};
    std::vector<std::thread> drainers;
    for (int i = 0; i != 2; ++i) {
      drainers.emplace_back([&] {
        while (!done.load(std::memory_order_acquire)) {
          wal->flush();
          std::this_thread::yield();
        }
      });
    }
    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer != kProducers; ++producer) {
      producers.emplace_back([&, producer] {
        const auto key_text = std::to_string(producer);
        const auto key_view = as_bytes(key_text);
        const auto value_view = as_bytes("value");
        for (std::uint64_t sequence = 1; sequence <= kRecords; ++sequence) {
          wal->append(persistence::MvccWalRecordView{
              std::as_bytes(key_view), sequence, std::as_bytes(value_view)});
        }
      });
    }
    for (auto& producer : producers) producer.join();
    done.store(true, std::memory_order_release);
    for (auto& drainer : drainers) drainer.join();
    wal->sync();
  }
  {
    constexpr std::size_t kProducers = 8;
    constexpr std::uint64_t kRecords = 1000;
    auto recovery = persistence::MvccWal::recover(concurrent_path);
    const auto records = recovery.records();
    assert(records.size() == kProducers * kRecords);
    std::vector<std::uint64_t> last(kProducers);
    for (const auto& record : records) {
      const std::string key_text(
          reinterpret_cast<const char*>(record.key.data()), record.key.size());
      const auto producer = static_cast<std::size_t>(std::stoul(key_text));
      assert(producer < kProducers);
      assert(record.timestamp == last[producer] + 1);
      last[producer] = record.timestamp;
    }
    for (const auto sequence : last) assert(sequence == kRecords);
  }
  std::filesystem::remove_all(path);
  std::cout << "WAL tests passed\n";
}
