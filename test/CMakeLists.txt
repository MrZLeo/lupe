cmake_minimum_required(VERSION 3.25)

#
# Project details
#

# Keep the parent's project scope so target helpers use Lupe's options.

verbose_message("Adding tests under ${CMAKE_PROJECT_NAME}Tests...")

CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  VERSION 3.11.0
  OPTIONS
    "BUILD_TESTING OFF"
    "CATCH_INSTALL_DOCS OFF"
    "CATCH_INSTALL_EXTRAS ON"
)

# Catch2's compiled library and the C++23 tests must agree on C++17
# standard-library support so StringMaker<std::string_view> is defined.
target_compile_features(Catch2 PRIVATE cxx_std_17)

if(DEFINED Catch2_SOURCE_DIR)
  set(CATCH2_EXTRAS_DIR "${Catch2_SOURCE_DIR}/extras")
elseif(DEFINED catch2_SOURCE_DIR)
  set(CATCH2_EXTRAS_DIR "${catch2_SOURCE_DIR}/extras")
else()
  message(FATAL_ERROR "Catch2 extras directory could not be determined.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CATCH2_EXTRAS_DIR}")
include(Catch)

foreach(file ${test_sources})
  string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp)" "\\2" test_name ${file})
  add_executable(${test_name}_Tests ${file})

  project_apply_cxx_standard(${test_name}_Tests PRIVATE)
  # Test executables keep exceptions enabled: Catch2 implements SKIP() and
  # assertion unwinding via exceptions thrown across test frames. They still
  # link against the exception-free ${CMAKE_PROJECT_NAME} targets.

  if(${CMAKE_PROJECT_NAME}_BUILD_EXECUTABLE)
    set(${CMAKE_PROJECT_NAME}_TEST_LIB ${CMAKE_PROJECT_NAME}_LIB)
  else()
    set(${CMAKE_PROJECT_NAME}_TEST_LIB ${CMAKE_PROJECT_NAME})
  endif()

  #
  # Setup code coverage if enabled
  #

  project_enable_coverage(${test_name}_Tests)

  target_link_libraries(
    ${test_name}_Tests
    PRIVATE
      Catch2::Catch2WithMain
      ${${CMAKE_PROJECT_NAME}_TEST_LIB}
  )

  if(test_name STREQUAL "line_frame_test")
    target_sources(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src/line_frame.cpp"
    )
    target_include_directories(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(
      ${test_name}_Tests
      PRIVATE
        ftxui::dom
    )
  elseif(test_name STREQUAL "inline_images_test")
    target_sources(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src/inline_images.cpp"
        "${CMAKE_SOURCE_DIR}/src/line_frame.cpp"
    )
    target_include_directories(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(${test_name}_Tests PRIVATE ftxui::dom)
  elseif(test_name STREQUAL "message_overview_test")
    target_sources(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src/message_overview.cpp"
    )
    target_include_directories(
      ${test_name}_Tests
      PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )
    target_link_libraries(
      ${test_name}_Tests
      PRIVATE
        ftxui::dom
    )
  endif()

  catch_discover_tests(${test_name}_Tests)
endforeach()

verbose_message("Finished adding unit tests for ${CMAKE_PROJECT_NAME}.")
