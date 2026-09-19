#include "hermesdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* err) {
  fprintf(stderr, "%s\n", err != NULL ? err : "unknown error");
  hermesdb_free(err);
  return 1;
}

int main(void) {
  hermesdb_options_t options;
  hermesdb_options_init(&options);
  options.enable_wal = 1;

  char* err = NULL;
  hermesdb_t* db = hermesdb_open("hello-c.db", &options, &err);
  if (db == NULL) {
    return fail(err);
  }

  const char* key = "greeting";
  const char* value = "hello, HermesDB";
  hermesdb_put(db, key, strlen(key), value, strlen(value), &err);
  if (err != NULL) {
    hermesdb_close(db);
    return fail(err);
  }

  size_t value_len = 0;
  char* found = hermesdb_get(db, key, strlen(key), &value_len, &err);
  if (err != NULL) {
    hermesdb_close(db);
    return fail(err);
  }
  if (found != NULL) {
    fwrite(found, 1, value_len, stdout);
    fputc('\n', stdout);
    hermesdb_free(found);
  }

  hermesdb_close(db);
  return 0;
}
