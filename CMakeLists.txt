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

# Source files
set(SOURCES
    src/main.cpp
    src/bencode.cpp
    src/torrent.cpp
    src/tracker.cpp
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

# Create executable
add_executable(gush ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(gush PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Link libraries
target_link_libraries(gush PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    pthread
)

# Compiler warnings
target_compile_options(gush PRIVATE -Wall -Wextra -Wpedantic)

# Install target
install(TARGETS gush DESTINATION bin)

# Test executable for magnet link
add_executable(test_magnet src/test_magnet.cpp src/magnet.cpp src/utils.cpp)
target_include_directories(test_magnet PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_magnet PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Unit tests with GoogleTest
set(TEST_SOURCES
    tests/test_bencode.cpp
    tests/test_utils.cpp
    tests/test_magnet.cpp
    tests/test_torrent.cpp
    tests/test_tracker.cpp
)

set(TEST_EXTERNAL_SOURCES
    src/bencode.cpp
    src/torrent.cpp
    src/tracker.cpp
    src/peer.cpp
    src/downloader.cpp
    src/utils.cpp
    src/magnet.cpp
    src/metadata.cpp
    src/tracker_list.cpp
)

add_executable(test_gush ${TEST_SOURCES} ${TEST_EXTERNAL_SOURCES})
target_include_directories(test_gush PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_gush PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    pthread
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_gush)
