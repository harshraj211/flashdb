include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(flashdb_tests
    test_parser.cpp
    test_storage.cpp
    test_expiry.cpp
    test_pubsub.cpp
    test_transactions.cpp
)

target_link_libraries(flashdb_tests PRIVATE
    flashdb_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(flashdb_tests)
