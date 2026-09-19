#include "hermesdb.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path make_dir(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void require_ok(char* err) {
  assert(err == nullptr);
  (void)err;
}

}  // namespace

int main() {
  const auto path = make_dir("hermesdb-capi");
  hermesdb_options_t options;
  hermesdb_options_init(&options);
  options.enable_wal = 1;
  options.compaction = HERMESDB_COMPACTION_NONE;

  char* err = nullptr;
  hermesdb_t* db = hermesdb_open(path.string().c_str(), &options, &err);
  require_ok(err);
  assert(db != nullptr);

  const char* key = "k";
  const char* value = "v";
  hermesdb_put(db, key, 1, value, 1, &err);
  require_ok(err);

  size_t value_len = 0;
  char* found = hermesdb_get(db, key, 1, &value_len, &err);
  require_ok(err);
  assert(found != nullptr);
  assert(value_len == 1);
  assert(found[0] == 'v');
  hermesdb_free(found);

  hermesdb_delete(db, key, 1, &err);
  require_ok(err);
  found = hermesdb_get(db, key, 1, &value_len, &err);
  require_ok(err);
  assert(found == nullptr);

  hermesdb_put(db, key, 1, value, 1, &err);
  require_ok(err);
  hermesdb_sync(db, &err);
  require_ok(err);
  hermesdb_close(db);

  db = hermesdb_open(path.string().c_str(), &options, &err);
  require_ok(err);
  found = hermesdb_get(db, key, 1, &value_len, &err);
  require_ok(err);
  assert(found != nullptr);
  assert(std::string(found, value_len) == "v");
  hermesdb_free(found);
  hermesdb_close(db);

  std::filesystem::remove_all(path);
  return 0;
}
