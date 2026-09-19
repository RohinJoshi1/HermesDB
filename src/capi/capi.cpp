#include "hermesdb.h"

#include "hermesdb/compaction.hpp"
#include "hermesdb/db.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

struct hermesdb_db {
  std::shared_ptr<hermesdb::DB> db;
};

namespace {

void set_error(char** errp, const char* message) {
  if (errp == nullptr) {
    return;
  }
  const auto size = std::strlen(message) + 1;
  auto* copy = static_cast<char*>(std::malloc(size));
  if (copy == nullptr) {
    *errp = nullptr;
    return;
  }
  std::memcpy(copy, message, size);
  *errp = copy;
}

void clear_error(char** errp) {
  if (errp != nullptr) {
    *errp = nullptr;
  }
}

hermesdb::Options to_options(const hermesdb_options_t* options) {
  hermesdb::Options out;
  if (options == nullptr) {
    return out;
  }
  if (options->block_size != 0) {
    out.block_size = options->block_size;
  }
  if (options->target_sst_size != 0) {
    out.target_sst_size = options->target_sst_size;
  }
  if (options->num_memtable_limit != 0) {
    out.num_memtable_limit = options->num_memtable_limit;
  }
  out.enable_wal = options->enable_wal != 0;
  out.serializable = options->serializable != 0;
  switch (options->compaction) {
    case HERMESDB_COMPACTION_NONE:
      out.compaction_options = hermesdb::NoCompactionOptions{};
      break;
    case HERMESDB_COMPACTION_SIMPLE:
      out.compaction_options = hermesdb::SimpleLeveledCompactionOptions{};
      break;
    case HERMESDB_COMPACTION_TIERED:
      out.compaction_options = hermesdb::TieredCompactionOptions{};
      break;
    case HERMESDB_COMPACTION_LEVELED:
    default:
      out.compaction_options = hermesdb::LeveledCompactionOptions{};
      break;
  }
  return out;
}

}  // namespace

extern "C" {

void hermesdb_options_init(hermesdb_options_t* options) {
  if (options == nullptr) {
    return;
  }
  *options = hermesdb_options_t{};
  options->block_size = 4096;
  options->target_sst_size = 2U << 20U;
  options->num_memtable_limit = 3;
  options->enable_wal = 0;
  options->serializable = 0;
  options->compaction = HERMESDB_COMPACTION_LEVELED;
}

hermesdb_t* hermesdb_open(const char* path, const hermesdb_options_t* options,
                          char** errp) {
  clear_error(errp);
  if (path == nullptr) {
    set_error(errp, "path must not be null");
    return nullptr;
  }
  try {
    auto* handle = new hermesdb_db;
    handle->db = hermesdb::DB::Open(path, to_options(options));
    return handle;
  } catch (const std::exception& ex) {
    set_error(errp, ex.what());
    return nullptr;
  }
}

void hermesdb_close(hermesdb_t* db) {
  if (db == nullptr) {
    return;
  }
  try {
    db->db->Close();
  } catch (...) {
  }
  delete db;
}

void hermesdb_put(hermesdb_t* db, const char* key, size_t key_len,
                  const char* value, size_t value_len, char** errp) {
  clear_error(errp);
  if (db == nullptr || key == nullptr || (value == nullptr && value_len != 0)) {
    set_error(errp, "invalid argument");
    return;
  }
  try {
    db->db->Put(std::string_view(key, key_len),
                std::string_view(value, value_len));
  } catch (const std::exception& ex) {
    set_error(errp, ex.what());
  }
}

char* hermesdb_get(hermesdb_t* db, const char* key, size_t key_len,
                   size_t* value_len, char** errp) {
  clear_error(errp);
  if (value_len != nullptr) {
    *value_len = 0;
  }
  if (db == nullptr || key == nullptr) {
    set_error(errp, "invalid argument");
    return nullptr;
  }
  try {
    const auto found = db->db->Get(std::string_view(key, key_len));
    if (!found) {
      return nullptr;
    }
    auto* copy = static_cast<char*>(std::malloc(found->size() + 1));
    if (copy == nullptr) {
      set_error(errp, "out of memory");
      return nullptr;
    }
    if (!found->empty()) {
      std::memcpy(copy, found->data(), found->size());
    }
    copy[found->size()] = '\0';
    if (value_len != nullptr) {
      *value_len = found->size();
    }
    return copy;
  } catch (const std::exception& ex) {
    set_error(errp, ex.what());
    return nullptr;
  }
}

void hermesdb_delete(hermesdb_t* db, const char* key, size_t key_len,
                     char** errp) {
  clear_error(errp);
  if (db == nullptr || key == nullptr) {
    set_error(errp, "invalid argument");
    return;
  }
  try {
    db->db->Delete(std::string_view(key, key_len));
  } catch (const std::exception& ex) {
    set_error(errp, ex.what());
  }
}

void hermesdb_sync(hermesdb_t* db, char** errp) {
  clear_error(errp);
  if (db == nullptr) {
    set_error(errp, "invalid argument");
    return;
  }
  try {
    db->db->Sync();
  } catch (const std::exception& ex) {
    set_error(errp, ex.what());
  }
}

void hermesdb_free(void* ptr) { std::free(ptr); }

}  // extern "C"
