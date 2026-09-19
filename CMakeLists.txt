cmake_minimum_required(VERSION 3.21)
project(HermesDB VERSION 0.1.0 LANGUAGES C CXX)

option(HERMESDB_BUILD_TESTS "Build HermesDB tests" ON)
option(HERMESDB_BUILD_APPS "Build CLI and benchmark binaries" ON)
option(HERMESDB_BUILD_EXAMPLES "Build API examples" ON)
option(HERMESDB_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
set(HERMESDB_SANITIZER "" CACHE STRING "Sanitizer: address, undefined, thread, or address-undefined")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(hermesdb
  src/common/common.cpp
  src/key/key.cpp
  src/memtable/memtable.cpp
  src/iterators/iterators.cpp
  src/storage/db.cpp
  src/block/block.cpp
  src/table/table.cpp
  src/compaction/compaction.cpp
  src/compaction/simple_leveled.cpp
  src/compaction/leveled.cpp
  src/compaction/tiered.cpp
  src/persistence/persistence.cpp
  src/capi/capi.cpp
)
add_library(hermesdb::hermesdb ALIAS hermesdb)

target_compile_features(hermesdb PRIVATE cxx_std_20)
target_include_directories(hermesdb
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
find_package(Threads REQUIRED)
target_link_libraries(hermesdb PUBLIC Threads::Threads)
set_target_properties(hermesdb PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  EXPORT_NAME hermesdb
)

if(HERMESDB_SANITIZER AND NOT MSVC)
  if(HERMESDB_SANITIZER STREQUAL "address-undefined")
    set(_hermesdb_sanitizers "address,undefined")
  elseif(HERMESDB_SANITIZER MATCHES "^(address|undefined|thread)$")
    set(_hermesdb_sanitizers "${HERMESDB_SANITIZER}")
  else()
    message(FATAL_ERROR "Unsupported HERMESDB_SANITIZER=${HERMESDB_SANITIZER}")
  endif()
  target_compile_options(hermesdb PUBLIC
    -fsanitize=${_hermesdb_sanitizers}
    -fno-omit-frame-pointer
  )
  target_link_options(hermesdb PUBLIC -fsanitize=${_hermesdb_sanitizers})
endif()

if(HERMESDB_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(hermesdb PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hermesdb PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endif()

if(HERMESDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HERMESDB_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(HERMESDB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

install(TARGETS hermesdb
  EXPORT HermesDBTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/hermesdb DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/hermesdb.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT HermesDBTargets
  FILE HermesDBTargets.cmake
  NAMESPACE hermesdb::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HermesDB
)

configure_package_config_file(
  cmake/HermesDBConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/HermesDBConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HermesDB
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/HermesDBConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/HermesDBConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/HermesDBConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HermesDB
)
