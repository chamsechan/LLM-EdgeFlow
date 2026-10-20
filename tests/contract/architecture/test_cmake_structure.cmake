cmake_minimum_required(VERSION 3.19)
if(NOT PROJECT_SOURCE_DIR)
  get_filename_component(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

# 只读取仓库文件；不配置工程、不下载依赖，也不需要编译器或 SDK。
file(READ "${PROJECT_SOURCE_DIR}/CMakeLists.txt" root)
string(REGEX REPLACE "#[^\n]*" "" root "${root}")
string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*[ \t\n]*\\(" commands "${root}")
set(shell_commands cmake_minimum_required if message endif project include)
foreach(command IN LISTS commands)
  string(REGEX REPLACE "[ \t\n]*\\($" "" command "${command}")
  if(NOT command IN_LIST shell_commands)
    message(FATAL_ERROR "Root CMakeLists.txt must remain an assembly shell: ${command}")
  endif()
endforeach()

# 检查模块装配顺序，不绑定目标定义的行号或具体格式。
string(REGEX MATCHALL "cmake_ext/[A-Za-z0-9_./]+\\.cmake" modules "${root}")
set(expected_modules
    cmake_ext/BackendOptions.cmake
    cmake_ext/ProjectSetup.cmake
    cmake_ext/Dependencies.cmake
    cmake_ext/RuntimeGraph.cmake
    cmake_ext/ProductTargets.cmake)
if(NOT modules STREQUAL expected_modules)
  message(FATAL_ERROR "Unexpected root module assembly: ${modules}")
endif()

file(GLOB_RECURSE modules "${PROJECT_SOURCE_DIR}/cmake_ext/*.cmake")
foreach(module IN LISTS modules)
  file(READ "${module}" content)
  string(REGEX REPLACE "#[^\n]*" "" content "${content}")
  if(content MATCHES "DH_[A-Za-z_]+|\\$\\{(platform|sdk)\\}|cmake/(utils|common)\\.cmake")
    message(FATAL_ERROR "Project modules must not infer internal host build context: ${module}")
  endif()
endforeach()

# 包括源码子目录中的项目模块引用，重命名后不能遗留失效 include。
file(GLOB_RECURSE source_lists
    "${PROJECT_SOURCE_DIR}/src/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/demo/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/dev_support/CMakeLists.txt")
list(APPEND modules ${source_lists} "${PROJECT_SOURCE_DIR}/CMakeLists.txt")
foreach(module IN LISTS modules)
  file(READ "${module}" content)
  string(REGEX REPLACE "#[^\n]*" "" content "${content}")
  string(REGEX MATCHALL "include[ \t\n]*\\([ \t\n]*\"?(\\$\\{PROJECT_SOURCE_DIR\\}/)?cmake_ext/[A-Za-z0-9_./]+\\.cmake"
      includes "${content}")
  foreach(reference IN LISTS includes)
    string(REGEX MATCH "cmake_ext/[A-Za-z0-9_./]+\\.cmake$" path "${reference}")
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${path}")
      message(FATAL_ERROR "Missing module ${path}, included by ${module}")
    endif()
  endforeach()
endforeach()

file(READ "${PROJECT_SOURCE_DIR}/cmake_ext/Dependencies.cmake" dependencies)
string(REGEX REPLACE "#[^\n]*" "" dependencies "${dependencies}")
string(REGEX REPLACE "include[ \t\n]*\\([^)]*\\)" "" dependencies "${dependencies}")
string(STRIP "${dependencies}" dependencies)
if(dependencies)
  message(FATAL_ERROR "Dependencies.cmake must only aggregate dependency modules")
endif()
message(STATUS "PASS CMake assembly and module references")
