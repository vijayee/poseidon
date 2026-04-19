cmake_minimum_required(VERSION 3.16)

project(Poseidon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Dependencies
add_subdirectory(deps/googletest)

# Build libcbor separately as C (sanitizers disabled to avoid link issues)
set(SANITIZE OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libcbor)

# Build hashmap
add_subdirectory(deps/hashmap)

# Build msquic (requires OpenSSL on Linux, disable -Werror for clang)
find_package(OpenSSL REQUIRED)
set(QUIC_DISABLE_WERROR ON CACHE BOOL "" FORCE)
add_subdirectory(deps/msquic)

# Source directories
set(MERIDIAN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Network/Meridian)
set(UTIL_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Util)
set(REFCOUNTER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/RefCounter)
set(WORKERS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Workers)
set(TIME_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Time)
set(BUFFER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Buffer)
set(BLOOM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Bloom)

# Poseidon Library
set(POSEIDON_SOURCES
    ${MERIDIAN_SRC_DIR}/meridian.c
    ${MERIDIAN_SRC_DIR}/meridian_ring.c
    ${MERIDIAN_SRC_DIR}/meridian_packet.c
    ${MERIDIAN_SRC_DIR}/meridian_measure.c
    ${MERIDIAN_SRC_DIR}/meridian_query.c
    ${MERIDIAN_SRC_DIR}/meridian_gossip.c
    ${MERIDIAN_SRC_DIR}/meridian_rendv.c
    ${MERIDIAN_SRC_DIR}/meridian_protocol.c
    ${UTIL_SRC_DIR}/allocator.c
    ${UTIL_SRC_DIR}/log.c
    ${UTIL_SRC_DIR}/vec.c
    ${UTIL_SRC_DIR}/mkdir_p.c
    ${UTIL_SRC_DIR}/path_join.c
    ${UTIL_SRC_DIR}/rm_rf.c
    ${UTIL_SRC_DIR}/threadding.c
    ${REFCOUNTER_SRC_DIR}/refcounter.c
    ${WORKERS_SRC_DIR}/pool.c
    ${WORKERS_SRC_DIR}/work.c
    ${WORKERS_SRC_DIR}/promise.c
    ${WORKERS_SRC_DIR}/queue.c
    ${WORKERS_SRC_DIR}/error.c
    ${WORKERS_SRC_DIR}/join.c
    ${WORKERS_SRC_DIR}/transaction_id.c
    ${TIME_SRC_DIR}/wheel.c
    ${TIME_SRC_DIR}/ticker.c
    ${TIME_SRC_DIR}/debouncer.c
    ${BUFFER_SRC_DIR}/buffer.c
    ${BLOOM_SRC_DIR}/bitset.c
    ${BLOOM_SRC_DIR}/bloom_filter.c
    ${BLOOM_SRC_DIR}/elastic_bloom_filter.c
    ${BLOOM_SRC_DIR}/attenuated_bloom_filter.c
)

add_library(poseidon STATIC ${POSEIDON_SOURCES})

target_include_directories(poseidon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
)

target_link_libraries(poseidon PRIVATE
    cbor
    hashmap
    pthread
    msquic::msquic
    msquic::platform
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Test include paths for transitive dependencies
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/hashmap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor/src
)

function(add_meridian_test TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${TEST_NAME} PRIVATE
        poseidon
        cbor
        hashmap
        msquic::msquic
        msquic::platform
        gtest
        gtest_main
        pthread
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Meridian Tests
enable_testing()

add_meridian_test(meridian_node_test tests/meridian_node_test.cpp)
add_meridian_test(meridian_ring_test tests/meridian_ring_test.cpp)
add_meridian_test(meridian_packet_test tests/meridian_packet_test.cpp)
add_meridian_test(meridian_measure_test tests/meridian_measure_test.cpp)
add_meridian_test(meridian_query_test tests/meridian_query_test.cpp)
add_meridian_test(meridian_integration_test tests/meridian_integration_test.cpp)
add_meridian_test(bloom_bitset_test tests/bloom_bitset_test.cpp)
add_meridian_test(bloom_filter_test tests/bloom_filter_test.cpp)
add_meridian_test(elastic_bloom_filter_test tests/elastic_bloom_filter_test.cpp)
add_meridian_test(attenuated_bloom_filter_test tests/attenuated_bloom_filter_test.cpp)