file(GLOB TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(fuse_test ${TEST_SRCS})
target_link_libraries(fuse_test PRIVATE GTest::gtest_main fuse)

include(GoogleTest)
gtest_discover_test(fuse_test)
