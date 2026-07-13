add_executable(PowerButtonGestureTest
  PowerButtonGestureTest.cpp
)

target_include_directories(PowerButtonGestureTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(PowerButtonGestureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerButtonGestureTest)
