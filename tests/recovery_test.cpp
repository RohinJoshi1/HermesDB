#include "hermesdb/hermesdb.hpp"
#include "hermesdb/key.hpp"
#include "hermesdb/persistence.hpp"
#include "hermesdb/table.hpp"

#include <cassert>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace hermesdb;

namespace {
persistence::Bytes pbytes(std::string_view text) {
  const auto* begin = reinterpret_cast<const std::byte*>(text.data());
  return {begin, begin + text.size()};
}

template <class Function>
void throws(Function&& function) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception&) {
    failed = true;
  }
  assert(failed);
}
}  // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("hermesdb-recovery-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(path);
  const auto wal_path = path / "batch.wal";
  {
    auto wal = persistence::Wal::create(wal_path);
    const std::vector<persistence::WalRecord> batch{
        {pbytes("a"), pbytes("one")}, {pbytes("b"), {}}};
    wal->append_batch(batch);
    wal->sync();
  }
  const auto valid_size = std::filesystem::file_size(wal_path);
  {
    std::ofstream output(wal_path, std::ios::binary | std::ios::app);
    output.write("\0\0x", 3);
  }
  auto recovered = persistence::Wal::recover(wal_path);
  assert(recovered.truncated_tail);
  assert(recovered.records.size() == 2);
  assert(std::filesystem::file_size(wal_path) == valid_size);

  Options options;
  options.enable_wal = true;
  options.compaction_options = NoCompactionOptions{};
  const auto db_path = path / "db";
  {
    auto db = DB::Open(db_path, options);
    const std::vector<WriteBatchRecord> batch{
        PutRecord{Bytes{'a'}, Bytes{'1'}},
        DeleteRecord{Bytes{'a'}},
        PutRecord{Bytes{'b'}, Bytes{'2'}}};
    db->WriteBatch(batch);
    db->Sync();
    db->Close();
  }
  {
    auto db = DB::Open(db_path, options);
    assert(!db->Get("a"));
    assert(as_string(*db->Get("b")) == "2");
    db->Close();
  }

  TableBuilder builder(64);
  builder.add(InternalKey("key"), as_bytes("value"));
  auto corrupt = builder.finish();
  corrupt[0] ^= 0xffU;
  auto table = Table::open(corrupt);
  throws([&] { (void)table->read_block(0); });

  const auto manifest_path = db_path / "MANIFEST";
  {
    std::fstream file(manifest_path,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(8);
    const char byte = '!';
    file.write(&byte, 1);
  }
  throws([&] { (void)persistence::Manifest::recover(manifest_path); });
  std::filesystem::remove_all(path);
  std::cout << "Recovery tests passed\n";
}
