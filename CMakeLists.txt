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

# Build BLAKE3
set(BLAKE3_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BLAKE3_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/blake3/c)

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
set(QUASAR_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Network/Quasar)
set(CRYPTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Crypto)
set(CHANNEL_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/Channel)

# Poseidon Library
set(POSEIDON_SOURCES
    ${MERIDIAN_SRC_DIR}/meridian.c
    ${MERIDIAN_SRC_DIR}/meridian_ring.c
    ${MERIDIAN_SRC_DIR}/meridian_packet.c
    ${MERIDIAN_SRC_DIR}/meridian_measure.c
    ${MERIDIAN_SRC_DIR}/meridian_query.c
    ${MERIDIAN_SRC_DIR}/meridian_gossip.c
    ${MERIDIAN_SRC_DIR}/meridian_rendv.c
    ${MERIDIAN_SRC_DIR}/meridian_relay.c
    ${MERIDIAN_SRC_DIR}/meridian_relay_server.c
    ${MERIDIAN_SRC_DIR}/meridian_conn.c
    ${MERIDIAN_SRC_DIR}/meridian_protocol.c
    ${UTIL_SRC_DIR}/allocator.c
    ${UTIL_SRC_DIR}/memory_pool.c
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
    ${QUASAR_SRC_DIR}/quasar.c
    ${QUASAR_SRC_DIR}/quasar_message_id.c
    ${QUASAR_SRC_DIR}/quasar_route.c
    ${UTIL_SRC_DIR}/base58.c
    ${UTIL_SRC_DIR}/hash.c
    ${CRYPTO_SRC_DIR}/blake3_wrapper.c
    ${CRYPTO_SRC_DIR}/node_id.c
    ${CRYPTO_SRC_DIR}/key_pair.c
    ${CHANNEL_SRC_DIR}/channel.c
    ${CHANNEL_SRC_DIR}/channel_manager.c
    ${CHANNEL_SRC_DIR}/subtopic.c
)

add_library(poseidon STATIC ${POSEIDON_SOURCES})

target_include_directories(poseidon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/blake3/c
)

target_link_libraries(poseidon PRIVATE
    cbor
    hashmap
    pthread
    blake3
    msquic::msquic
    msquic::platform
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Meridian Relay Server
add_executable(meridian_relay
    src/relay_main.c
    src/Network/Meridian/meridian_relay_server.c
)

target_include_directories(meridian_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/blake3/c
)

target_link_libraries(meridian_relay PRIVATE
    poseidon
    cbor
    hashmap
    blake3
    msquic::msquic
    msquic::platform
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Test include paths for transitive dependencies
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/hashmap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcbor/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/blake3/c
)

function(add_meridian_test TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${TEST_NAME} PRIVATE
        poseidon
        cbor
        hashmap
        blake3
        msquic::msquic
        msquic::platform
        OpenSSL::SSL
        OpenSSL::Crypto
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
add_meridian_test(meridian_relay_test tests/meridian_relay_test.cpp)
add_meridian_test(meridian_nat_detect_test tests/meridian_nat_detect_test.cpp)
add_meridian_test(meridian_conn_test tests/meridian_conn_test.cpp)
add_meridian_test(bloom_bitset_test tests/bloom_bitset_test.cpp)
add_meridian_test(bloom_filter_test tests/bloom_filter_test.cpp)
add_meridian_test(elastic_bloom_filter_test tests/elastic_bloom_filter_test.cpp)
add_meridian_test(attenuated_bloom_filter_test tests/attenuated_bloom_filter_test.cpp)
add_meridian_test(quasar_test tests/quasar_test.cpp)

# PKI Tests
add_meridian_test(pki_test tests/pki_test.cpp)

# Channel Tests
add_meridian_test(channel_test tests/channel_test.cpp)
