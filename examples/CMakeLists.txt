add_executable(hermesdb_hello_cpp hello.cpp)
target_link_libraries(hermesdb_hello_cpp PRIVATE hermesdb::hermesdb)
target_compile_features(hermesdb_hello_cpp PRIVATE cxx_std_20)

add_executable(hermesdb_hello_c hello.c)
target_link_libraries(hermesdb_hello_c PRIVATE hermesdb::hermesdb)

if(HERMESDB_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(hermesdb_hello_cpp PRIVATE /W4 /permissive-)
    target_compile_options(hermesdb_hello_c PRIVATE /W4)
  else()
    target_compile_options(hermesdb_hello_cpp PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion)
    target_compile_options(hermesdb_hello_c PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
