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
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF CACHE BOOL "" FORCE)
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

# Build poll-dancer (event loop library)
set(POLL_DANCER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(POLL_DANCER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/poll-dancer)

# Build libwtf (WebTransport over HTTP/3) using our existing msquic
set(WTF_USE_EXTERNAL_MSQUIC ON CACHE BOOL "" FORCE)
set(WTF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WTF_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(msquic_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake CACHE PATH "msquic config dir" FORCE)
add_subdirectory(deps/libwtf)

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
set(CLIENT_APIS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientAPIs)
set(CLIENT_LIBS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c)

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
    ${MERIDIAN_SRC_DIR}/msquic_singleton.c
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
    ${CHANNEL_SRC_DIR}/channel_message.c
    ${CHANNEL_SRC_DIR}/channel_notice.c
    ${CHANNEL_SRC_DIR}/topic_alias.c
    ${CHANNEL_SRC_DIR}/topic_id.c
    ${CLIENT_APIS_SRC_DIR}/client_protocol.c
    ${CLIENT_APIS_SRC_DIR}/client_session.c
    ${CLIENT_APIS_SRC_DIR}/transport_unix.c
    ${CLIENT_APIS_SRC_DIR}/transport_tcp.c
    ${CLIENT_APIS_SRC_DIR}/transport_ws.c
    ${CLIENT_APIS_SRC_DIR}/transport_quic.c
    ${CLIENT_APIS_SRC_DIR}/transport_webtransport.c
    ${CLIENT_LIBS_SRC_DIR}/poseidon_client.c
)

# Platform-specific transport stubs
if(ANDROID)
    list(APPEND POSEIDON_SOURCES ${CLIENT_APIS_SRC_DIR}/transport_binder.c)
endif()
if(APPLE)
    list(APPEND POSEIDON_SOURCES ${CLIENT_APIS_SRC_DIR}/transport_xpc.c)
endif()

add_library(poseidon STATIC ${POSEIDON_SOURCES})

target_include_directories(poseidon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/blake3/c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libwtf/include
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
    poll_dancer
    wtf
)

# Meridian Relay Server
set(RELAY_STANDALONE OFF)
set(POSEIDON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/Network/Meridian/relay ${CMAKE_BINARY_DIR}/meridian_relay)

# Poseidon Daemon
add_executable(poseidond src/poseidond.c)

target_include_directories(poseidond PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/msquic/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxhash
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/blake3/c
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/libwtf/include
)

target_link_libraries(poseidond PRIVATE
    poseidon
    cbor
    hashmap
    blake3
    msquic::msquic
    msquic::platform
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
    poll_dancer
    wtf
    m
)

# ============================================================================
# Poseidon Client Library (separate installable target)
# ============================================================================

# Built via its own CMakeLists.txt for standalone builds, or add_subdirectory here
# When building as part of the parent project, source dirs and cbor target are available.
set(POSEIDON_CLIENT_STANDALONE OFF)
set(POSEIDON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(POSEIDON_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c)
set(POSEIDON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ClientLibs/c ${CMAKE_BINARY_DIR}/poseidon_client)

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
add_meridian_test(channel_bootstrap_test tests/channel_bootstrap_test.cpp)
add_meridian_test(channel_notice_test tests/channel_notice_test.cpp)
add_meridian_test(topic_id_test tests/topic_id_test.cpp)

# ClientAPI Tests
add_meridian_test(client_protocol_test tests/client_protocol_test.cpp)
add_meridian_test(client_session_test tests/client_session_test.cpp)
add_meridian_test(poseidon_client_test tests/poseidon_client_test.cpp)
