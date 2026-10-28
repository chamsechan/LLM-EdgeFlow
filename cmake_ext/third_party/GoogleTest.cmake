# Google Test：从固定版本源码构建。

include(FetchContent)
include(cmake_ext/third_party/ThirdPartyDownload.cmake)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
