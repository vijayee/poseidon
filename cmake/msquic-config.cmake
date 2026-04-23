# Shim msquic package config for in-tree builds.
# The msquic target is created by add_subdirectory(deps/msquic) in the
# parent project. This file satisfies find_package(msquic CONFIG) from
# sub-projects like libwtf that need to use our already-built msquic.

if(NOT TARGET msquic::msquic)
    message(FATAL_ERROR "msquic::msquic target not found — ensure deps/msquic is built first")
endif()

set(msquic_FOUND TRUE)
set(MSQUIC_LIBRARIES msquic)