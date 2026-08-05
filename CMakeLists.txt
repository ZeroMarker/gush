cmake_minimum_required(VERSION 3.14)
project(gush VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set default build type to Release for better performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optimization flags for Release build
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Enable link-time optimization for better performance
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Enable testing
enable_testing()

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Library source files
set(GUSH_LIB_SOURCES
    src/bencode.cpp
    src/torrent.cpp
    src/tracker.cpp
    src/tracker_manager.cpp
    src/peer.cpp
    src/downloader.cpp
    src/utils.cpp
    src/magnet.cpp
    src/metadata.cpp
    src/tracker_list.cpp
)

set(HEADERS
    include/bencode.h
    include/torrent.h
    include/tracker.h
    include/peer.h
    include/downloader.h
    include/utils.h
    include/magnet.h
    include/metadata.h
    include/tracker_list.h
)

add_library(gush_lib STATIC ${GUSH_LIB_SOURCES} ${HEADERS})
target_include_directories(gush_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gush_lib PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    pthread
)
target_compile_options(gush_lib PRIVATE -Wall -Wextra -Wpedantic)

# Create executable
add_executable(gush src/main.cpp)
target_link_libraries(gush PRIVATE gush_lib)

# Compiler warnings
set(GUSH_WARNING_FLAGS -Wall -Wextra -Wpedantic)
target_compile_options(gush PRIVATE ${GUSH_WARNING_FLAGS})

# Install target
install(TARGETS gush DESTINATION bin)

# Test executable for magnet link
# (legacy standalone test removed: covered by tests/test_magnet.cpp)

# Unit tests with GoogleTest
set(TEST_SOURCES
    tests/test_bencode.cpp
    tests/test_utils.cpp
    tests/test_magnet.cpp
    tests/test_torrent.cpp
    tests/test_tracker.cpp
    tests/test_tracker_manager.cpp
    tests/test_downloader.cpp
)

add_executable(test_gush ${TEST_SOURCES})
target_compile_definitions(test_gush PRIVATE GUSH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(test_gush PRIVATE ${GUSH_WARNING_FLAGS})
target_link_libraries(test_gush PRIVATE
    gush_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_gush)
