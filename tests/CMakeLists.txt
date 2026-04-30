cmake_minimum_required(VERSION 3.14)

# --- Test executable ---
add_executable(zmqae_tests
    test_message.cpp
    test_uuid.cpp
    test_client.cpp
    test_router.cpp
    test_integration.cpp
    test_c_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/c_api.cpp
)

target_link_libraries(zmqae_tests PRIVATE zmqae)
target_include_directories(zmqae_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../extern/doctest/doctest
)

# doctest header-only: just need include path
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN is defined in one test file

add_test(NAME zmqae_tests COMMAND zmqae_tests)
