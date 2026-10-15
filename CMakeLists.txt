cmake_minimum_required(VERSION 3.25)

#
# Project details
#

project(
  "Loupe"
  VERSION 0.6.0
  LANGUAGES CXX
)

#
# Set project options
#
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "") # works

include(cmake/StandardSettings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Utils.cmake)
include(cmake/CPM.cmake)
include(cmake/ProjectStandard.cmake)

project_configure_cxx_standard(23)
message(STATUS "Started CMake for ${PROJECT_NAME} v${PROJECT_VERSION}...\n")

if (UNIX)
    add_compile_options("$<$<CONFIG:DEBUG>:-D_DEBUG>")    #this will allow to use same _DEBUG macro available in both Linux as well as Windows - MSCV environment. Easy to put Debug specific code.
endif (UNIX)

# Select the MSVC runtime before creating any project or dependency targets.
# Callers can override this with -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
# when producing a standalone Windows executable.
if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()


#
# Setup alternative names
#

if(${PROJECT_NAME}_USE_ALT_NAMES)
  string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWERCASE)
  string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPERCASE)
else()
  set(PROJECT_NAME_LOWERCASE ${PROJECT_NAME})
  set(PROJECT_NAME_UPPERCASE ${PROJECT_NAME})
endif()

#
# Prevent building in the source directory
#

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.\n")
endif()

#
# Create library, setup header and source files
#

# Find all headers and implementation files
include(cmake/SourcesAndHeaders.cmake)

if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  add_executable(${PROJECT_NAME} ${exe_sources})

  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following sources:")
    foreach(source IN LISTS exe_sources)
      verbose_message("* ${source}")
    endforeach()
  endif()

  if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
    add_library(${PROJECT_NAME}_LIB ${headers} ${sources})

    if(${PROJECT_NAME}_VERBOSE_OUTPUT)
      verbose_message("Found the following headers:")
      foreach(header IN LISTS headers)
        verbose_message("* ${header}")
      endforeach()
    endif()
  endif()
elseif(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  add_library(${PROJECT_NAME} INTERFACE)

  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following headers:")
    foreach(header IN LISTS headers)
      verbose_message("* ${header}")
    endforeach()
  endif()
else()
  add_library(
    ${PROJECT_NAME}
    ${headers}
    ${sources}
  )

  if(${PROJECT_NAME}_VERBOSE_OUTPUT)
    verbose_message("Found the following sources:")
    foreach(source IN LISTS sources)
      verbose_message("* ${source}")
    endforeach()
    verbose_message("Found the following headers:")
    foreach(header IN LISTS headers)
      verbose_message("* ${header}")
    endforeach()
  endif()
endif()

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/$<CONFIG>"
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/$<CONFIG>"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
)
if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME loupe)
endif()
if(${PROJECT_NAME}_BUILD_EXECUTABLE AND ${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  set_target_properties(
    ${PROJECT_NAME}_LIB
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/$<CONFIG>"
    OUTPUT_NAME ${PROJECT_NAME}
  )
endif()

message(STATUS "Added all header and implementation files.\n")

#
# Set the project standard and warnings
#

if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  project_apply_cxx_standard(${PROJECT_NAME} INTERFACE)
else()
  project_apply_cxx_standard(${PROJECT_NAME} PUBLIC)

  if(${PROJECT_NAME}_BUILD_EXECUTABLE AND ${PROJECT_NAME}_ENABLE_UNIT_TESTING)
    project_apply_cxx_standard(${PROJECT_NAME}_LIB PUBLIC)
  endif()
endif()
include(cmake/CompilerWarnings.cmake)
foreach(project_target IN ITEMS ${PROJECT_NAME} ${PROJECT_NAME}_LIB)
  if(TARGET ${project_target})
    set_project_warnings(${project_target})
    project_enable_static_analysis(${project_target})
    project_enable_coverage(${project_target})
  endif()
endforeach()
verbose_message("Applied compiler warnings. Using standard C++${PROJECT_CXX_STANDARD}.\n")

#
# Disable C++ exceptions for all first-party targets
#

include(cmake/DisableExceptions.cmake)
project_disable_exceptions(${PROJECT_NAME})
if(${PROJECT_NAME}_BUILD_EXECUTABLE AND ${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  project_disable_exceptions(${PROJECT_NAME}_LIB)
endif()

if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  # Force argum into its Expected-based API so parse() reports errors via
  # return values in both exception and no-exception builds.
  target_compile_definitions(${PROJECT_NAME} PRIVATE ARGUM_USE_EXPECTED=1)
endif()
verbose_message("Disabled C++ exceptions for project targets.\n")

#
# Enable Doxygen
#

include(cmake/Doxygen.cmake)

#
# Model project dependencies
#

set(PROJECT_INSTALL_SIMDJSON OFF)
if(NOT ${PROJECT_NAME}_BUILD_EXECUTABLE
   AND NOT ${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  set(PROJECT_INSTALL_SIMDJSON ON)
endif()

CPMAddPackage(
  NAME simdjson
  GITHUB_REPOSITORY simdjson/simdjson
  VERSION 4.6.6
  OPTIONS
    "SIMDJSON_INSTALL ${PROJECT_INSTALL_SIMDJSON}"
    "SIMDJSON_BUILD_EXAMPLES OFF"
    "SIMDJSON_BUILD_TESTS OFF"
    "SIMDJSON_BUILD_BENCHMARKS OFF"
)

# The patch step must be idempotent: reconfiguring an existing build tree
# re-runs it against an already-patched source, and plain `patch` fails on
# reversed hunks. `-N` skips already-applied hunks (answering "no" to the
# reversal prompt via EOF on stdin) and exits nonzero, hence the `|| true`.
CPMAddPackage(
  NAME FTXUI
  GITHUB_REPOSITORY ArthurSonzogni/FTXUI
  VERSION 7.0.2
  PATCH_COMMAND patch -p1 -N -r /dev/null < "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/ftxui-terminal-input-burst.patch" || true
  OPTIONS
    "FTXUI_BUILD_DOCS OFF"
    "FTXUI_BUILD_EXAMPLES OFF"
    "FTXUI_BUILD_TESTS OFF"
    "FTXUI_ENABLE_INSTALL OFF"
)

CPMAddPackage(
  NAME argum
  GITHUB_REPOSITORY gershnik/argum
  GIT_TAG v2.11
  OPTIONS
    "ARGUM_NO_TESTS ON"
)

# Memory-only image decoding for Kitty's PNG/RGBA wire formats. A pinned
# header-only dependency keeps releases independent of system image libraries.
CPMAddPackage(
  NAME stb
  GITHUB_REPOSITORY nothings/stb
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  DOWNLOAD_ONLY YES
)
foreach(image_target IN ITEMS ${PROJECT_NAME} ${PROJECT_NAME}_LIB)
  if(TARGET ${image_target} AND NOT ${PROJECT_NAME}_BUILD_HEADERS_ONLY)
    target_include_directories(${image_target} SYSTEM PRIVATE "${stb_SOURCE_DIR}")
  endif()
endforeach()

# Vendored, decompression-only static libzstd: session logs may be stored
# as concatenated Zstandard frames (deepseek-harness session.jsonl.zstd),
# and a pinned in-tree build keeps release binaries dependency-free on
# macOS, Ubuntu, and MSVC.
CPMAddPackage(
  NAME zstd
  GITHUB_REPOSITORY facebook/zstd
  GIT_TAG v1.5.7
  SOURCE_SUBDIR build/cmake
  OPTIONS
    "ZSTD_BUILD_PROGRAMS OFF"
    "ZSTD_BUILD_TESTS OFF"
    "ZSTD_BUILD_CONTRIB OFF"
    "ZSTD_BUILD_SHARED OFF"
    "ZSTD_BUILD_STATIC ON"
    "ZSTD_BUILD_COMPRESSION OFF"
    "ZSTD_BUILD_DECOMPRESSION ON"
    "ZSTD_BUILD_DICTBUILDER OFF"
    "ZSTD_BUILD_DEPRECATED OFF"
    "ZSTD_LEGACY_SUPPORT OFF"
    "ZSTD_MULTITHREAD_SUPPORT OFF"
)
if(TARGET libzstd_static)
  target_include_directories(
    libzstd_static SYSTEM
    PUBLIC $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>
  )
endif()

function(project_mark_dependency_includes_system target)
  if(NOT TARGET ${target})
    return()
  endif()

  get_target_property(dependency_includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(dependency_includes)
    set_target_properties(
      ${target}
      PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dependency_includes}"
    )
  endif()
endfunction()

foreach(dependency_target IN ITEMS simdjson screen dom component argum libzstd_static)
  project_mark_dependency_includes_system(${dependency_target})
endforeach()

set(PROJECT_LIBRARY_DEPENDENCIES simdjson::simdjson libzstd_static)
set(PROJECT_EXECUTABLE_DEPENDENCIES simdjson::simdjson ftxui::component argum::argum libzstd_static)

function(project_link_dependencies target)
  get_target_property(target_type ${target} TYPE)
  if(target_type STREQUAL "INTERFACE_LIBRARY")
    return()
  elseif(target_type STREQUAL "EXECUTABLE")
    target_link_libraries(${target} PRIVATE ${PROJECT_EXECUTABLE_DEPENDENCIES})
  else()
    target_link_libraries(${target} PRIVATE ${PROJECT_LIBRARY_DEPENDENCIES})
  endif()
endfunction()

project_link_dependencies(${PROJECT_NAME})
if(${PROJECT_NAME}_BUILD_EXECUTABLE AND ${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  project_link_dependencies(${PROJECT_NAME}_LIB)
endif()

verbose_message("Successfully added all dependencies and linked against them.")

#
# Set the build/user include directories
#

# Allow usage of header files in the `src` directory, but only for utilities
if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
  target_include_directories(
    ${PROJECT_NAME}
    INTERFACE
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  )
else()
  target_include_directories(
    ${PROJECT_NAME}
    PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  if(${PROJECT_NAME}_BUILD_EXECUTABLE AND ${PROJECT_NAME}_ENABLE_UNIT_TESTING)
    target_include_directories(
      ${PROJECT_NAME}_LIB
      PUBLIC
      $<INSTALL_INTERFACE:include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
  endif()
endif()

message(STATUS "Finished setting up include directories.")

#
# Provide alias to library for
#

if(${PROJECT_NAME}_BUILD_EXECUTABLE)
  add_executable(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
else()
  add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
endif()

verbose_message("Project is now aliased as ${PROJECT_NAME}::${PROJECT_NAME}.\n")

#
# Format the project using the `clang-format` target (i.e: cmake --build build --target clang-format)
#

if(PROJECT_IS_TOP_LEVEL)
  add_clang_format_target()
endif()

#
# Install library for easy downstream inclusion
#

include(GNUInstallDirs)
install(
  TARGETS
  ${PROJECT_NAME}
  EXPORT
  ${PROJECT_NAME}Targets
  LIBRARY DESTINATION
  ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION
  ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION
  ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION
  include
  PUBLIC_HEADER DESTINATION
  include
)

install(
  EXPORT
  ${PROJECT_NAME}Targets
  FILE
  ${PROJECT_NAME}Targets.cmake
  NAMESPACE
  ${PROJECT_NAME}::
  DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

#
# Add version header
#

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/version.hpp.in
  include/${PROJECT_NAME_LOWERCASE}/version.hpp
  @ONLY
)

install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/include/${PROJECT_NAME_LOWERCASE}/version.hpp
  DESTINATION
  include/${PROJECT_NAME_LOWERCASE}
)

#
# Install the `include` directory
#

install(
  DIRECTORY
  include/${PROJECT_NAME_LOWERCASE}
  DESTINATION
  include
)

verbose_message("Install targets successfully built. Install with `cmake --build <build_directory> --target install --config <build_config>`.")

#
# Quick `ConfigVersion.cmake` creation
#

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${PROJECT_NAME}ConfigVersion.cmake
  VERSION
  ${PROJECT_VERSION}
  COMPATIBILITY
  SameMajorVersion
)

set(PROJECT_CONFIG_DEPENDENCIES "")
if(PROJECT_INSTALL_SIMDJSON)
  set(
    PROJECT_CONFIG_DEPENDENCIES
    "include(CMakeFindDependencyMacro)\nfind_dependency(simdjson CONFIG PATHS \"\${PACKAGE_PREFIX_DIR}\")"
  )
endif()

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/${PROJECT_NAME}Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

#
# Generate export header if specified
#

if(${PROJECT_NAME}_GENERATE_EXPORT_HEADER)
  include(GenerateExportHeader)
  generate_export_header(${PROJECT_NAME})
  install(
    FILES
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME_LOWERCASE}_export.h
    DESTINATION
    include
  )

  message(STATUS "Generated the export header `${PROJECT_NAME_LOWERCASE}_export.h` and installed it.")
endif()

message(STATUS "Finished building requirements for installing the package.\n")

#
# Unit testing setup
#

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  enable_testing()
  message(STATUS "Build unit tests for the project. Tests should always be found in the test folder\n")
  add_subdirectory(test)
endif()
