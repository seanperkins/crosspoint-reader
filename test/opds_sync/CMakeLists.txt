add_executable(OpdsSyncEngineTest
  OpdsSyncEngineTest.cpp
  ${REPO_ROOT}/src/network/OpdsSyncEngine.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(OpdsSyncEngineTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(OpdsSyncEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsSyncEngineTest)
