# Google Test (GTest) 智能缓存与预编译复用配置

option(GTEST_FORCE_REBUILD "Force rebuilding googletest even if cached in 3rdparty" OFF)
include(cmake_ext/third_party/ThirdPartyCacheMetadata.cmake)
edgeflow_prepare_third_party_cache(
  NAME googletest
  VERSION 1.14.0
  SOURCE_SHA256 8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
  CACHE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/googletest"
  KIND STATIC
  ABI_OPTIONS BUILD_GMOCK=OFF
  REQUIRED_FILES lib/libgtest.a lib/libgtest_main.a include/gtest/gtest.h
  OUT_CACHE_DIR GTEST_3RDPARTY_DIR
  OUT_VALID _GTEST_CACHE_VALID
  OUT_MARKER _GTEST_CACHE_MARKER)

if(_GTEST_CACHE_VALID AND NOT GTEST_FORCE_REBUILD)
  message(STATUS "[3rdparty] Using cached GoogleTest from ${GTEST_3RDPARTY_DIR}")
  find_package(Threads REQUIRED)
  add_library(GTest::gtest STATIC IMPORTED GLOBAL)
  set_target_properties(GTest::gtest PROPERTIES
    IMPORTED_LOCATION "${GTEST_3RDPARTY_DIR}/lib/libgtest.a"
    INTERFACE_INCLUDE_DIRECTORIES "${GTEST_3RDPARTY_DIR}/include"
    INTERFACE_LINK_LIBRARIES Threads::Threads)
  add_library(GTest::gtest_main STATIC IMPORTED GLOBAL)
  set_target_properties(GTest::gtest_main PROPERTIES
    IMPORTED_LOCATION "${GTEST_3RDPARTY_DIR}/lib/libgtest_main.a"
    INTERFACE_INCLUDE_DIRECTORIES "${GTEST_3RDPARTY_DIR}/include"
    INTERFACE_LINK_LIBRARIES GTest::gtest)
else()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    ${EDGEFLOW_FETCHCONTENT_TIMESTAMP_ARGS}
  )
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  edgeflow_publish_third_party_cache(googletest
    CACHE_DIR "${GTEST_3RDPARTY_DIR}" MARKER "${_GTEST_CACHE_MARKER}"
    DEPENDS gtest gtest_main
    LIBRARIES "$<TARGET_FILE:gtest>" "$<TARGET_FILE:gtest_main>"
    HEADER_DIRS "${googletest_SOURCE_DIR}/googletest/include")
endif()

include(GoogleTest)
