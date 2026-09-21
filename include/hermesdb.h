#ifndef HERMESDB_H
#define HERMESDB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hermesdb_db hermesdb_t;

enum hermesdb_compaction {
  HERMESDB_COMPACTION_NONE = 0,
  HERMESDB_COMPACTION_SIMPLE = 1,
  HERMESDB_COMPACTION_LEVELED = 2,
  HERMESDB_COMPACTION_TIERED = 3
};

typedef struct hermesdb_options {
  size_t block_size;
  size_t target_sst_size;
  size_t num_memtable_limit;
  size_t block_cache_capacity;
  int enable_wal;
  int serializable;
  int compaction;
} hermesdb_options_t;

void hermesdb_options_init(hermesdb_options_t* options);

hermesdb_t* hermesdb_open(const char* path, const hermesdb_options_t* options,
                          char** errp);
void hermesdb_close(hermesdb_t* db);

void hermesdb_put(hermesdb_t* db, const char* key, size_t key_len,
                  const char* value, size_t value_len, char** errp);
char* hermesdb_get(hermesdb_t* db, const char* key, size_t key_len,
                   size_t* value_len, char** errp);
void hermesdb_delete(hermesdb_t* db, const char* key, size_t key_len,
                     char** errp);
void hermesdb_sync(hermesdb_t* db, char** errp);

void hermesdb_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
