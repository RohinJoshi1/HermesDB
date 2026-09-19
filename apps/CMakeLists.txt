add_executable(hermesdb_bench bench/main.cpp)
target_link_libraries(hermesdb_bench PRIVATE hermesdb::hermesdb)
target_compile_features(hermesdb_bench PRIVATE cxx_std_20)

if(HERMESDB_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(hermesdb_bench PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hermesdb_bench PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endif()

if(HERMESDB_BUILD_TESTS)
  add_test(NAME hermesdb_bench_smoke
    COMMAND hermesdb_bench
      --workload fillrandom
      --num 200
      --threads 1
      --json
      --no-wal
      --compaction none)
  set_tests_properties(hermesdb_bench_smoke PROPERTIES
    LABELS "extra;bench"
  )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/main.cpp")
  add_executable(hermesdb_cli cli/main.cpp)
  target_link_libraries(hermesdb_cli PRIVATE hermesdb::hermesdb)
  target_compile_features(hermesdb_cli PRIVATE cxx_std_20)
  if(HERMESDB_ENABLE_WARNINGS)
    if(MSVC)
      target_compile_options(hermesdb_cli PRIVATE /W4 /permissive-)
    else()
      target_compile_options(hermesdb_cli PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
    endif()
  endif()
endif()
