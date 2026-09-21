#include "hermesdb/table.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace hermesdb;

int main() {
  TableBuilder packed(4096, Compression::zlib);
  TableBuilder raw(4096, Compression::none);
  for (int index = 0; index < 200; ++index) {
    const auto key = "key-" + std::string(3 - std::to_string(index).size(), '0') +
                     std::to_string(index);
    const std::string value(80, 'a');
    packed.add(InternalKey(key), as_bytes(value));
    raw.add(InternalKey(key), as_bytes(value));
  }
  const auto packed_bytes = packed.finish();
  const auto raw_bytes = raw.finish();
  assert(packed_bytes.size() < raw_bytes.size());
  assert(packed.raw_block_bytes() == raw.raw_block_bytes());

  auto table = Table::open(packed_bytes);
  assert(table->num_blocks() >= 1);
  for (int index = 0; index < 200; ++index) {
    const auto key = "key-" + std::string(3 - std::to_string(index).size(), '0') +
                     std::to_string(index);
    const auto found = table->get(as_bytes(key));
    assert(found);
    assert(as_string(*found) == std::string(80, 'a'));
  }

  const auto path =
      std::filesystem::temp_directory_path() /
      ("hermesdb-sst-packed-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".sst");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(packed_bytes.data()),
              static_cast<std::streamsize>(packed_bytes.size()));
    assert(out);
  }
  auto file_table = Table::open(path);
  assert(file_table->file_size() == packed_bytes.size());
  assert(as_string(*file_table->get(as_bytes("key-000"))) == std::string(80, 'a'));
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  std::cout << "Packed compression tests passed\n";
}
