# Cross-platform compiler-cache discovery without hard-coded executable or
# storage paths. Users and CI remain responsible for CCACHE_DIR policy.

function(edgeflow_paths_resolve_to_same_file left_path right_path output_var)
  if(NOT left_path OR NOT right_path OR
     NOT EXISTS "${left_path}" OR NOT EXISTS "${right_path}")
    set(${output_var} FALSE PARENT_SCOPE)
    return()
  endif()

  get_filename_component(left_real_path "${left_path}" REALPATH)
  get_filename_component(right_real_path "${right_path}" REALPATH)
  if(left_real_path STREQUAL right_real_path)
    set(${output_var} TRUE PARENT_SCOPE)
  else()
    set(${output_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(edgeflow_enable_compiler_cache)
  if(NOT LLM_EDGEFLOW_USE_CCACHE)
    return()
  endif()

  find_program(LLM_EDGEFLOW_CCACHE_EXECUTABLE ccache)
  if(NOT LLM_EDGEFLOW_CCACHE_EXECUTABLE)
    message(STATUS "ccache not found; compiler caching disabled")
    return()
  endif()

  foreach(language C CXX)
    if(CMAKE_${language}_COMPILER_LAUNCHER)
      message(STATUS
        "Respecting existing ${language} compiler launcher: "
        "${CMAKE_${language}_COMPILER_LAUNCHER}")
      continue()
    endif()

    edgeflow_paths_resolve_to_same_file(
      "${CMAKE_${language}_COMPILER}"
      "${LLM_EDGEFLOW_CCACHE_EXECUTABLE}"
      compiler_is_ccache_wrapper)
    if(compiler_is_ccache_wrapper)
      message(STATUS
        "${language} compiler already resolves to ccache; launcher skipped")
    else()
      set(CMAKE_${language}_COMPILER_LAUNCHER
          "${LLM_EDGEFLOW_CCACHE_EXECUTABLE}" PARENT_SCOPE)
      message(STATUS
        "Found ccache for ${language}: ${LLM_EDGEFLOW_CCACHE_EXECUTABLE}")
    endif()
  endforeach()
endfunction()
