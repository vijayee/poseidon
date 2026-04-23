cmake_minimum_required(VERSION 3.16)

project(PoseidonClient VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Dependencies
# ============================================================================

# When built as part of the parent project, cbor and OpenSSL are already found.
# When built standalone, we need to find them ourselves.

if(NOT TARGET cbor)
    # Standalone build: find libcbor via pkg-config, bundled, or system
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBCBOR IMPORTED_TARGET libcbor)
    endif()
    if(LIBCBOR_FOUND)
        set(CBOR_TARGET PkgConfig::LIBCBOR)
    else()
        # Try to find libcbor in a deps/ subdirectory relative to project root
        set(_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../deps)
        if(EXISTS ${_DEPS_DIR}/libcbor/CMakeLists.txt)
            set(SANITIZE OFF CACHE BOOL "" FORCE)
            add_subdirectory(${_DEPS_DIR}/libcbor ${CMAKE_BINARY_DIR}/deps/libcbor)
            set(CBOR_TARGET cbor)
            set(CBOR_INCLUDE_DIR ${_DEPS_DIR}/libcbor/src)
        else()
            find_path(LIBCBOR_INCLUDE_DIR cbor.h)
            find_library(LIBCBOR_LIBRARY cbor)
            if(LIBCBOR_INCLUDE_DIR AND LIBCBOR_LIBRARY)
                add_library(libcbor_static STATIC IMPORTED)
                set_target_properties(libcbor_static PROPERTIES
                    IMPORTED_LOCATION ${LIBCBOR_LIBRARY}
                    INTERFACE_INCLUDE_DIRECTORIES ${LIBCBOR_INCLUDE_DIR}
                )
                set(CBOR_TARGET libcbor_static)
            else()
                message(FATAL_ERROR "libcbor not found. Install it, provide deps/libcbor/, or set CMAKE_PREFIX_PATH")
            endif()
        endif()
    endif()
else()
    set(CBOR_TARGET cbor)
endif()

if(NOT OpenSSL_FOUND)
    find_package(OpenSSL REQUIRED)
endif()

find_package(Threads REQUIRED)

# ============================================================================
# Source directories
# ============================================================================

# Source paths: standalone build uses relative paths from this directory,
# in-tree build uses paths passed from parent project.
if(POSEIDON_CLIENT_STANDALONE)
    # Built via add_subdirectory from parent project — paths set by parent
    # POSEIDON_SRC_DIR and POSEIDON_CLIENT_DIR are already defined
else()
    # Standalone build — compute paths relative to this CMakeLists.txt
    set(POSEIDON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    set(POSEIDON_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Public headers directory — contains include/poseidon/*.h
if(NOT DEFINED POSEIDON_INCLUDE_DIR)
    set(POSEIDON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
endif()

set(POSEIDON_CLIENT_SOURCES
    ${POSEIDON_CLIENT_DIR}/poseidon_client.c
    ${POSEIDON_SRC_DIR}/ClientAPIs/client_protocol.c
    ${POSEIDON_SRC_DIR}/Channel/channel_config_defaults.c
    ${POSEIDON_SRC_DIR}/Util/allocator.c
    ${POSEIDON_SRC_DIR}/Util/log.c
    ${POSEIDON_SRC_DIR}/Util/threadding.c
    ${POSEIDON_SRC_DIR}/Crypto/key_pair.c
    ${POSEIDON_SRC_DIR}/RefCounter/refcounter.c
    ${POSEIDON_SRC_DIR}/Network/Meridian/msquic_singleton.c
)

# ============================================================================
# Build targets
# ============================================================================

add_library(poseidon_client_objects OBJECT ${POSEIDON_CLIENT_SOURCES})
set_target_properties(poseidon_client_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(poseidon_client_objects PUBLIC
    ${POSEIDON_SRC_DIR}
    ${POSEIDON_INCLUDE_DIR}
)

# msquic include directory
if(EXISTS ${POSEIDON_SRC_DIR}/../deps/msquic/src/inc/msquic.h)
    target_include_directories(poseidon_client_objects PUBLIC
        ${POSEIDON_SRC_DIR}/../deps/msquic/src/inc
    )
endif()

# For libcbor headers: in-tree build uses deps/libcbor/src, standalone uses found package
if(TARGET cbor)
    # cbor target exists — get its include dir from the target
    target_link_libraries(poseidon_client_objects PUBLIC cbor)
    set_target_properties(cbor PROPERTIES POSITION_INDEPENDENT_CODE ON)
elseif(DEFINED CBOR_INCLUDE_DIR)
    target_include_directories(poseidon_client_objects PUBLIC ${CBOR_INCLUDE_DIR})
elseif(DEFINED LIBCBOR_INCLUDE_DIR)
    target_include_directories(poseidon_client_objects PUBLIC ${LIBCBOR_INCLUDE_DIR})
endif()

target_link_libraries(poseidon_client_objects PUBLIC
    ${CBOR_TARGET}
    OpenSSL::Crypto
    Threads::Threads
)

# Link msquic if available
if(TARGET msquic::msquic)
    target_link_libraries(poseidon_client_objects PUBLIC msquic::msquic msquic::platform)
endif()

add_library(poseidon_client_static STATIC $<TARGET_OBJECTS:poseidon_client_objects>)
set_target_properties(poseidon_client_static PROPERTIES OUTPUT_NAME poseidon_client)
target_include_directories(poseidon_client_static PUBLIC
    $<BUILD_INTERFACE:${POSEIDON_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(poseidon_client_static PUBLIC
    ${CBOR_TARGET}
    OpenSSL::Crypto
    Threads::Threads
)

if(TARGET msquic::msquic)
    target_link_libraries(poseidon_client_static PUBLIC msquic::msquic msquic::platform)
endif()

add_library(poseidon_client_shared SHARED $<TARGET_OBJECTS:poseidon_client_objects>)
set_target_properties(poseidon_client_shared PROPERTIES OUTPUT_NAME poseidon_client)
target_include_directories(poseidon_client_shared PUBLIC
    $<BUILD_INTERFACE:${POSEIDON_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(poseidon_client_shared PUBLIC
    ${CBOR_TARGET}
    OpenSSL::Crypto
    Threads::Threads
)

if(TARGET msquic::msquic)
    target_link_libraries(poseidon_client_shared PUBLIC msquic::msquic msquic::platform)
endif()

# ============================================================================
# Install rules
# ============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS poseidon_client_static poseidon_client_shared
    EXPORT PoseidonClientTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    ${POSEIDON_INCLUDE_DIR}/poseidon/poseidon_client.h
    ${POSEIDON_INCLUDE_DIR}/poseidon/poseidon_key_pair.h
    ${POSEIDON_INCLUDE_DIR}/poseidon/poseidon_channel_config.h
    ${POSEIDON_INCLUDE_DIR}/poseidon/poseidon_refcounter.h
    ${POSEIDON_INCLUDE_DIR}/poseidon/poseidon_atomic_compat.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/poseidon
)

install(EXPORT PoseidonClientTargets
    FILE PoseidonClientTargets.cmake
    NAMESPACE PoseidonClient::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PoseidonClient
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PoseidonClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PoseidonClientConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PoseidonClient
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/PoseidonClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/PoseidonClientConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/PoseidonClientConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PoseidonClient
)