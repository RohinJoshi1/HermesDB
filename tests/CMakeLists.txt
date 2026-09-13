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
  LABELS "course;week1;week1-day1"
)

add_executable(tiny_lsm_week1_day2_tests course/week1_day2_test.cpp)
target_link_libraries(tiny_lsm_week1_day2_tests PRIVATE tiny_lsm::tiny_lsm)
target_compile_features(tiny_lsm_week1_day2_tests PRIVATE cxx_std_20)

if(TINY_LSM_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(tiny_lsm_week1_day2_tests
      PRIVATE /W4 /permissive-
    )
  else()
    target_compile_options(tiny_lsm_week1_day2_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
  endif()
endif()

add_test(NAME tiny_lsm_week1_day2 COMMAND tiny_lsm_week1_day2_tests)
set_tests_properties(tiny_lsm_week1_day2 PROPERTIES
  LABELS "course;week1;week1-day2"
)

add_executable(tiny_lsm_week1_day3_tests course/week1_day3_test.cpp)
target_link_libraries(tiny_lsm_week1_day3_tests PRIVATE tiny_lsm::tiny_lsm)
target_compile_features(tiny_lsm_week1_day3_tests PRIVATE cxx_std_20)

if(TINY_LSM_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(tiny_lsm_week1_day3_tests
      PRIVATE /W4 /permissive-
    )
  else()
    target_compile_options(tiny_lsm_week1_day3_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    )
  endif()
endif()

add_test(NAME tiny_lsm_week1_day3 COMMAND tiny_lsm_week1_day3_tests)
set_tests_properties(tiny_lsm_week1_day3 PROPERTIES
  LABELS "course;week1;week1-day3"
)

function(tiny_lsm_add_week1_test day)
  add_executable(tiny_lsm_week1_day${day}_tests course/week1_day${day}_test.cpp)
  target_link_libraries(tiny_lsm_week1_day${day}_tests PRIVATE tiny_lsm::tiny_lsm)
  target_compile_features(tiny_lsm_week1_day${day}_tests PRIVATE cxx_std_20)
  if(TINY_LSM_ENABLE_WARNINGS)
    if(MSVC)
      target_compile_options(tiny_lsm_week1_day${day}_tests
        PRIVATE /W4 /permissive-
      )
    else()
      target_compile_options(tiny_lsm_week1_day${day}_tests
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion
      )
    endif()
  endif()
  add_test(NAME tiny_lsm_week1_day${day} COMMAND tiny_lsm_week1_day${day}_tests)
  set_tests_properties(tiny_lsm_week1_day${day} PROPERTIES
    LABELS "course;week1;week1-day${day}"
  )
endfunction()

tiny_lsm_add_week1_test(4)
tiny_lsm_add_week1_test(5)
tiny_lsm_add_week1_test(6)
tiny_lsm_add_week1_test(7)
set_tests_properties(tiny_lsm_week1_day7 PROPERTIES
  LABELS "course;week1;week1-day7;current"
)
