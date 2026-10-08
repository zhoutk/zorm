# zorm_bundle_runtime_deps.cmake - POST_BUILD helper, run via `cmake -P`.
#
# Copies every non-system DLL the given executable needs next to it, so an AI
# client (VS Code, Claude Code, ...) can spawn the server without inheriting an
# MSYS2 clang64 PATH. Without this, a client started from Explorer fails with
#   Connection state: Error Process exited with code 3221225781
# (0xC0000135 STATUS_DLL_NOT_FOUND) because libc++.dll / libmariadb.dll only
# exist in the toolchain bin dir - the backend DLLs alone are not enough.
#
# Scalar inputs (generator expressions are already expanded by the caller):
#   ZORM_TARGET_EXE      the freshly linked executable
#   ZORM_THIRDS_DLL_DIR  thirds/bin/win10 (backend DLLs; empty when absent)
#   ZORM_COMPILER_BIN    toolchain bin dir (libc++.dll, libunwind.dll, ...)
#
# A dependency that cannot be resolved fails the build: shipping an executable
# that dies at load time is worse than a build error.

if(NOT ZORM_TARGET_EXE)
  message(FATAL_ERROR "zorm_bundle_runtime_deps: ZORM_TARGET_EXE is required")
endif()

if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

get_filename_component(_out_dir "${ZORM_TARGET_EXE}" DIRECTORY)
get_filename_component(_exe_name "${ZORM_TARGET_EXE}" NAME)

# ---- candidate DLLs, in priority order --------------------------------------
# The exe dir first (previous builds), then the vendored backend DLLs, then the
# toolchain (libc++.dll, libunwind.dll, libmariadb.dll, ...).
set(_src_dirs "${_out_dir}")
if(ZORM_THIRDS_DLL_DIR AND EXISTS "${ZORM_THIRDS_DLL_DIR}")
  list(APPEND _src_dirs "${ZORM_THIRDS_DLL_DIR}")
endif()
if(ZORM_COMPILER_BIN AND EXISTS "${ZORM_COMPILER_BIN}")
  list(APPEND _src_dirs "${ZORM_COMPILER_BIN}")
endif()

# ---- staging area -----------------------------------------------------------
# Resolution runs against a private copy: a bare exe (no siblings) plus one flat
# lib dir holding every candidate under a unique name. GET_RUNTIME_DEPENDENCIES
# refuses to resolve a name found in two search locations, and the real exe dir
# would collide with the toolchain dir (both ship libiconv-2.dll).
string(MAKE_C_IDENTIFIER "${_exe_name}" _slug)
if(DEFINED ENV{TEMP})
  set(_stage "$ENV{TEMP}/zorm-rtd-${_slug}")
else()
  set(_stage "${_out_dir}/.zorm-rtd-${_slug}")
endif()
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}/bin" "${_stage}/libs")

foreach(_dir IN LISTS _src_dirs)
  file(GLOB _dlls "${_dir}/*.dll")
  foreach(_dll IN LISTS _dlls)
    get_filename_component(_name "${_dll}" NAME)
    if(NOT EXISTS "${_stage}/libs/${_name}")
      execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${_dll}" "${_stage}/libs")
    endif()
  endforeach()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${ZORM_TARGET_EXE}" "${_stage}/bin")

# ---- recursive closure ------------------------------------------------------
# A flat copy is not enough: the transitive dependencies of libpq/libmariadb
# must be found from the exe dir too.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${_stage}/bin/${_exe_name}"
    DIRECTORIES "${_stage}/libs"
    PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
    POST_EXCLUDE_REGEXES
        "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32"
        "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64"
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved)

# ---- publish ----------------------------------------------------------------
# Vendored backend DLLs are copied unconditionally (some are plugins the import
# table never mentions); the resolved closure is copied on top of them.
if(ZORM_THIRDS_DLL_DIR AND EXISTS "${ZORM_THIRDS_DLL_DIR}")
  file(GLOB _thirds_dlls "${ZORM_THIRDS_DLL_DIR}/*.dll")
  foreach(_dll IN LISTS _thirds_dlls)
    get_filename_component(_name "${_dll}" NAME)
    if(NOT EXISTS "${_out_dir}/${_name}")
      execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${_dll}" "${_out_dir}")
    endif()
  endforeach()
endif()

foreach(_dep IN LISTS _resolved)
  get_filename_component(_name "${_dep}" NAME)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                          "${_dep}" "${_out_dir}")
endforeach()

file(REMOVE_RECURSE "${_stage}")

if(_unresolved)
  message(FATAL_ERROR
      "zorm_bundle_runtime_deps: unresolved dependencies of ${ZORM_TARGET_EXE}:\n"
      "  ${_unresolved}\n"
      "Add the directory holding them to ZORM_THIRDS_DLL_DIR / ZORM_COMPILER_BIN.")
endif()

list(LENGTH _resolved _n)
message(STATUS "zorm: bundled ${_n} resolved runtime DLL(s) for ${_exe_name}")
