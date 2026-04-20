#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libcbor::libcbor" for configuration "Release"
set_property(TARGET libcbor::libcbor APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libcbor::libcbor PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcbor.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS libcbor::libcbor )
list(APPEND _IMPORT_CHECK_FILES_FOR_libcbor::libcbor "${_IMPORT_PREFIX}/lib/libcbor.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
