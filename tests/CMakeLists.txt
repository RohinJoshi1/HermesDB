if(MSVC)
  add_compile_options(/UNDEBUG)
else()
  add_compile_options(-UNDEBUG)
endif()

add_executable(tiny_lsm_week1_day1_tests course/week1_day1_test.cpp)
target_link_libraries(tiny_lsm_week1_day1_tests PRIVATE tiny_lsm::tiny_lsm)
target_compile_features(tiny_lsm_week1_day1_tests PRIVATE cxx_std_20)

if(TINY_LSM_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(tiny_lsm_week1_day1_tests
      PRIVATE /W4 /permissive-
    )
  else()
    target_compile_options(tiny_lsm_week1_day1_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
  endif()
endif()

add_test(NAME tiny_lsm_week1_day1 COMMAND tiny_lsm_week1_day1_tests)
set_tests_properties(tiny_lsm_week1_day1 PROPERTIES
  LABELS "course;week1;week1-day1;current"
)
