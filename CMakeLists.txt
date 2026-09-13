cmake_minimum_required(VERSION 3.21)
project(tiny_lsm VERSION 0.1.0 LANGUAGES CXX)

option(TINY_LSM_BUILD_TESTS "Build tiny-lsm tests" ON)
option(TINY_LSM_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
set(TINY_LSM_SANITIZER "" CACHE STRING "Sanitizer: address, undefined, thread, or address-undefined")

add_library(tiny_lsm
  src/common/common.cpp
  src/memtable/memtable.cpp
  src/iterators/iterators.cpp
  src/storage/db.cpp
  src/block/block.cpp
  src/table/table.cpp
)
add_library(tiny_lsm::tiny_lsm ALIAS tiny_lsm)
add_library(tiny_lsm_mvcc ALIAS tiny_lsm)

target_compile_features(tiny_lsm PUBLIC cxx_std_20)
target_include_directories(tiny_lsm
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
find_package(Threads REQUIRED)
target_link_libraries(tiny_lsm PUBLIC Threads::Threads)

if(TINY_LSM_SANITIZER AND NOT MSVC)
  if(TINY_LSM_SANITIZER STREQUAL "address-undefined")
    set(_tiny_lsm_sanitizers "address,undefined")
  elseif(TINY_LSM_SANITIZER MATCHES "^(address|undefined|thread)$")
    set(_tiny_lsm_sanitizers "${TINY_LSM_SANITIZER}")
  else()
    message(FATAL_ERROR "Unsupported TINY_LSM_SANITIZER=${TINY_LSM_SANITIZER}")
  endif()
  target_compile_options(tiny_lsm PUBLIC
    -fsanitize=${_tiny_lsm_sanitizers}
    -fno-omit-frame-pointer
  )
  target_link_options(tiny_lsm PUBLIC -fsanitize=${_tiny_lsm_sanitizers})
endif()

if(TINY_LSM_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(tiny_lsm PRIVATE /W4 /permissive-)
  else()
    target_compile_options(tiny_lsm PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endif()

if(TINY_LSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
