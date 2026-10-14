cmake_minimum_required(VERSION 3.16)

project(MeridianRelay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Project deps/ directory (always needed for xxhash includes)
set(_RELAY_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../deps)

# msquic — required QUIC library.
# In-tree build: target already exists from parent project's add_subdirectory(deps/msquic).
# Standalone build: set MSQUIC_DIR to the directory containing msquic-config.cmake,
#   or build msquic first from the deps/ directory.

if(NOT TARGET msquic::msquic)
    # Check for in-tree deps/msquic first

    if(EXISTS ${_RELAY_DEPS_DIR}/msquic/CMakeLists.txt AND NOT MSQUIC_DIR)
        # Build msquic from source as part of this project
        set(QUIC_DISABLE_WERROR ON CACHE BOOL "" FORCE)
        find_package(OpenSSL REQUIRED)
        # Build OpenSSL for msquic if needed
        add_subdirectory(${_RELAY_DEPS_DIR}/msquic ${CMAKE_BINARY_DIR}/_deps/msquic)
        set(MSQUIC_TARGET msquic::msquic)
        set(MSQUIC_PLATFORM_TARGET msquic::platform)
        set(MSQUIC_INC_DIR ${_RELAY_DEPS_DIR}/msquic/src/inc)
    elseif(MSQUIC_DIR OR msquic_FOUND)
        # Use pre-built or system-installed msquic
        find_package(msquic REQUIRED)
        set(MSQUIC_TARGET msquic::msquic)
        set(MSQUIC_PLATFORM_TARGET msquic::platform)
    else()
        message(FATAL_ERROR
            "msquic not found. Options:\n"
            "  1. Build from in-tree deps/ (automatic if deps/msquic/ exists)\n"
            "  2. Set -DMSQUIC_DIR=/path/to/msquic-config.cmake/directory\n"
            "  3. Install msquic system-wide\n")
    endif()
else()
    # Already available from parent project
    set(MSQUIC_TARGET msquic::msquic)
    set(MSQUIC_PLATFORM_TARGET msquic::platform)
endif()

if(NOT OpenSSL_FOUND)
    find_package(OpenSSL REQUIRED)
endif()

find_package(Threads REQUIRED)

# ============================================================================
# Source directories
# ============================================================================

if(NOT DEFINED RELAY_STANDALONE)
    # Standalone build — compute paths relative to this CMakeLists.txt
    set(POSEIDON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()

set(MERIDIAN_RELAY_SOURCES
    ${POSEIDON_SRC_DIR}/relay_main.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_relay_server.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_stream.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/msquic_singleton.c
    ${POSEIDON_SRC_DIR}/RefCounter/refcounter.c
    ${POSEIDON_SRC_DIR}/Util/allocator.c
    ${POSEIDON_SRC_DIR}/Util/log.c
    ${POSEIDON_SRC_DIR}/Util/mkdir_p.c
    ${POSEIDON_SRC_DIR}/Util/path_join.c
    ${POSEIDON_SRC_DIR}/Util/rm_rf.c
    ${POSEIDON_SRC_DIR}/Platform/platform_atomic.c
    ${POSEIDON_SRC_DIR}/Platform/platform_dirs.c
    ${POSEIDON_SRC_DIR}/Platform/platform_file.c
    ${POSEIDON_SRC_DIR}/Platform/platform_local.c
    ${POSEIDON_SRC_DIR}/Platform/platform_posix_compat.c
    ${POSEIDON_SRC_DIR}/Platform/platform_process.c
    ${POSEIDON_SRC_DIR}/Platform/platform_random.c
    ${POSEIDON_SRC_DIR}/Platform/platform_socket.c
    ${POSEIDON_SRC_DIR}/Platform/platform_socket_watcher.c
    ${POSEIDON_SRC_DIR}/Platform/platform_thread.c
    ${POSEIDON_SRC_DIR}/Platform/platform_time.c
    ${POSEIDON_SRC_DIR}/Util/vec.c
    ${POSEIDON_SRC_DIR}/Util/hash.c
    ${POSEIDON_SRC_DIR}/Util/base58.c
    ${POSEIDON_SRC_DIR}/Util/memory_pool.c
    ${POSEIDON_SRC_DIR}/Buffer/buffer.c
    ${POSEIDON_SRC_DIR}/Workers/pool.c
    ${POSEIDON_SRC_DIR}/Workers/work.c
    ${POSEIDON_SRC_DIR}/Workers/promise.c
    ${POSEIDON_SRC_DIR}/Workers/queue.c
    ${POSEIDON_SRC_DIR}/Workers/error.c
    ${POSEIDON_SRC_DIR}/Workers/join.c
    ${POSEIDON_SRC_DIR}/Workers/transaction_id.c
    ${POSEIDON_SRC_DIR}/Actor/actor.c
    ${POSEIDON_SRC_DIR}/Actor/message_queue.c
    ${POSEIDON_SRC_DIR}/Scheduler/scheduler.c
    ${POSEIDON_SRC_DIR}/Scheduler/deque.c
    ${POSEIDON_SRC_DIR}/Time/wheel.c
    ${POSEIDON_SRC_DIR}/Time/ticker.c
    ${POSEIDON_SRC_DIR}/Time/debouncer.c
    ${POSEIDON_SRC_DIR}/Crypto/blake3_wrapper.c
    ${POSEIDON_SRC_DIR}/Crypto/node_id.c
    ${POSEIDON_SRC_DIR}/Crypto/key_pair.c
    ${POSEIDON_SRC_DIR}/Bloom/bitset.c
    ${POSEIDON_SRC_DIR}/Bloom/bloom_filter.c
    ${POSEIDON_SRC_DIR}/Bloom/elastic_bloom_filter.c
    ${POSEIDON_SRC_DIR}/Bloom/attenuated_bloom_filter.c
    ${POSEIDON_SRC_DIR}/Channel/channel.c
    ${POSEIDON_SRC_DIR}/Channel/channel_manager.c
    ${POSEIDON_SRC_DIR}/Channel/channel_message.c
    ${POSEIDON_SRC_DIR}/Channel/channel_notice.c
    ${POSEIDON_SRC_DIR}/Channel/subtopic.c
    ${POSEIDON_SRC_DIR}/Channel/topic_alias.c
    ${POSEIDON_SRC_DIR}/Channel/topic_id.c
    ${POSEIDON_SRC_DIR}/Network/Quasar/quasar.c
    ${POSEIDON_SRC_DIR}/Network/Quasar/quasar_message_id.c
    ${POSEIDON_SRC_DIR}/Network/Quasar/quasar_route.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_ring.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_packet.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_measure.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_query.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_gossip.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_rendv.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_relay.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_conn.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/meridian_protocol.c
)

# ============================================================================
# Build target
# ============================================================================

add_executable(meridian_relay ${MERIDIAN_RELAY_SOURCES})

target_include_directories(meridian_relay PRIVATE
    ${POSEIDON_SRC_DIR}
    ${POSEIDON_SRC_DIR}/../deps/poll-dancer/include
    ${POSEIDON_SRC_DIR}/deps/hashmap/include
    ${POSEIDON_SRC_DIR}/deps/libcbor/src
    ${POSEIDON_SRC_DIR}/deps/blake3/c
    ${POSEIDON_SRC_DIR}/deps/msquic/src/inc
    ${_RELAY_DEPS_DIR}/xxhash
)

# msquic include dir — only needed when building with in-tree deps
if(DEFINED MSQUIC_INC_DIR)
    target_include_directories(meridian_relay PRIVATE ${MSQUIC_INC_DIR})
endif()

target_link_libraries(meridian_relay PRIVATE
    ${MSQUIC_TARGET}
    ${MSQUIC_PLATFORM_TARGET}
    cbor
    hashmap
    blake3
    poll_dancer
    wtf
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    m
)

# ============================================================================
# Install
# ============================================================================

include(GNUInstallDirs)

install(TARGETS meridian_relay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})