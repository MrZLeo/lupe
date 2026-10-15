function(project_configure_cxx_standard required_standard)
  if(NOT "cxx_std_${required_standard}" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    message(
      FATAL_ERROR
      "Lupe requires C++${required_standard}, but compiler "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} does not advertise support."
    )
  endif()

  set(PROJECT_CXX_STANDARD ${required_standard} PARENT_SCOPE)
  message(STATUS "Using C++${required_standard}")
endfunction()

function(project_apply_cxx_standard target visibility)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Cannot apply the project C++ standard to unknown target `${target}`.")
  endif()

  set_target_properties(
    ${target}
    PROPERTIES
      CXX_STANDARD ${PROJECT_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
  )

  target_compile_features(${target} ${visibility} cxx_std_${PROJECT_CXX_STANDARD})
endfunction()
