cmake_minimum_required(VERSION 3.16)

project(PoseidonNode VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# cmake-js provides CMAKE_JS_INC, CMAKE_JS_SRC, etc.
# node-addon-api headers are in node_modules/node-addon-api

# Find node-addon-api
find_path(NAPI_INCLUDE_DIR
    NAMES napi.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api"
    NO_DEFAULT_PATH
)

if(NOT NAPI_INCLUDE_DIR)
    message(FATAL_ERROR "node-addon-api not found. Run: npm install")
endif()

# Build the C client library
set(POSEIDON_CLIENT_STANDALONE OFF)
set(POSEIDON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(POSEIDON_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(POSEIDON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../c ${CMAKE_BINARY_DIR}/poseidon_client)

# Build the native addon
add_library(poseidon_node_native SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/addon/poseidon_addon.cpp
)

target_include_directories(poseidon_node_native PRIVATE
    ${NAPI_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_link_libraries(poseidon_node_native PRIVATE
    poseidon_client_static
    OpenSSL::Crypto
    Threads::Threads
)

# Node addon naming convention: .node extension, no "lib" prefix
set_target_properties(poseidon_node_native PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    OUTPUT_NAME "poseidon_node_native"
)