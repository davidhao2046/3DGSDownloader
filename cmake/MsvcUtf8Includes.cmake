# A piped Chinese MSVC can emit UTF-8 while CMake's built-in AUTO probe decodes
# with the system ANSI code page. Detect using the same /utf-8 option as targets;
# otherwise Ninja silently misses header dependencies and prints every include.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/GsdlIncludes")
  file(MAKE_DIRECTORY "${_probe_dir}")
  file(WRITE "${_probe_dir}/probe.hpp" "\n")
  file(WRITE "${_probe_dir}/probe.cpp" "#include \"probe.hpp\"\n")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /c probe.cpp
    WORKING_DIRECTORY "${_probe_dir}"
    OUTPUT_VARIABLE _include_output ERROR_VARIABLE _include_errors
    RESULT_VARIABLE _include_result ENCODING UTF-8)
  if(NOT _include_result EQUAL 0)
    message(FATAL_ERROR "MSVC dependency probe failed: ${_include_errors}")
  endif()
  if(_include_output MATCHES "(^|\n)([^\r\n]+: +)[A-Za-z]:[/\\\\][^\r\n]*probe[.]hpp")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    message(STATUS "MSVC UTF-8 include prefix: ${CMAKE_CL_SHOWINCLUDES_PREFIX}")
  else()
    message(FATAL_ERROR "Cannot detect MSVC UTF-8 include prefix; header tracking is required")
  endif()
endif()
