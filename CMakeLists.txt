cmake_minimum_required(VERSION 3.10)
project(s3fifo_rocksdb)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link filesystem library if needed (for GCC < 9.1)
if(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    link_libraries(stdc++fs)
endif()

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find required packages
find_package(gflags REQUIRED)
find_package(Snappy REQUIRED)
find_package(BZip2 REQUIRED)

# Find lz4
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)

# Find zstd
pkg_check_modules(ZSTD REQUIRED libzstd)

# Find RocksDB
find_package(RocksDB)
if(NOT RocksDB_FOUND)
    message(STATUS "Could not find RocksDB package, trying pkg-config")
    pkg_check_modules(ROCKSDB REQUIRED rocksdb)
endif()
find_package(spdlog REQUIRED)

# Source files
set(SOURCES
    example.cpp
)

# Header files
set(HEADERS
    s3fifo_rocksdb.hpp
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE 
    rocksdb
    gflags
    Snappy::snappy
    BZip2::BZip2
    ${LZ4_LIBRARIES}
    ${ZSTD_LIBRARIES}
    spdlog::spdlog
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GFLAGS_INCLUDE_DIRS}
    ${ROCKSDB_INCLUDE_DIR}
    ${LZ4_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
)

# Installation rules
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Add testing support (optional)
option(BUILD_TESTING "Build tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    # Add test files here when needed
    add_test(NAME paper_example_test COMMAND ${PROJECT_NAME})
endif() 