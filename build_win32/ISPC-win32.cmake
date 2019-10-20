# ISPC-win32.cmake -- for Windows port of facebook360-dep 
# created 12 Oct 2019 TKSharpless@gmail.com
# ISPC source at https://sourceforge.net/projects/ispcmirror/ was last updated in 2015 
# Only one component of facebook360-dep -- convertToBinary -- uses ispc
# Building ispc from source is complex and hard to automate but the result is a simple dll
# So I have chosen to ship prebuilt dlls, import libraries and header with fb360-dep.
# This script makes them visible to generated projects

if(NOT DEFINED BUILD_WIN32_DIR)
  set(BUILD_WIN32_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build_win32)
endif()

set(TEXCOMP_DIR ${BUILD_WIN32_DIR}/ispc_texcomp)

add_library(
  ispc_texcomp SHARED IMPORTED GLOBAL
  ${TEXCOMP_DIR}/ispc_texcomp.h
)

set_target_properties(ispc_texcomp PROPERTIES IMPORTED_LOCATION_DEBUG ${TEXCOMP_DIR}/debug/ispc_texcomp.dll)
set_target_properties(ispc_texcomp PROPERTIES IMPORTED_IMPLIB_DEBUG ${TEXCOMP_DIR}/debug/ispc_texcomp.lib)
set_target_properties(ispc_texcomp PROPERTIES IMPORTED_LOCATION_RELEASE ${TEXCOMP_DIR}/release/ispc_texcomp.dll)
set_target_properties(ispc_texcomp PROPERTIES IMPORTED_IMPLIB_RELEASE ${TEXCOMP_DIR}/release/ispc_texcomp.lib)
