cmake_minimum_required(VERSION 3.14)
project(zmq-algebraiceffect VERSION 0.0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# --- Header-only library ---
add_library(zmqae INTERFACE)
target_include_directories(zmqae INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# --- Submodule dependencies ---
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/libzmq)
add_subdirectory(extern/cppzmq)

target_include_directories(zmqae INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/cppzmq
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/spdlog/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/libzmq/include
)

target_link_libraries(zmqae INTERFACE cppzmq libzmq)

# --- Tests ---
option(ZMQAE_BUILD_TESTS "Build tests" ON)
if(ZMQAE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Examples ---
option(ZMQAE_BUILD_EXAMPLES "Build examples" ON)
if(ZMQAE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
