# cmake/GoogleTest.cmake
# Google Test (GTest) 智能缓存与预编译复用配置

set(GTEST_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/googletest")
option(GTEST_FORCE_REBUILD "Force rebuilding googletest even if cached in 3rdparty" OFF)

if(EXISTS "${GTEST_3RDPARTY_DIR}/lib/libgtest.a" AND
   EXISTS "${GTEST_3RDPARTY_DIR}/lib/libgtest_main.a" AND
   EXISTS "${GTEST_3RDPARTY_DIR}/include/gtest/gtest.h" AND
   NOT GTEST_FORCE_REBUILD)
  message(STATUS "[3rdparty] Using cached GoogleTest from ${GTEST_3RDPARTY_DIR}")
  find_package(Threads REQUIRED)

  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest STATIC IMPORTED GLOBAL)
    set_target_properties(GTest::gtest PROPERTIES
      IMPORTED_LOCATION "${GTEST_3RDPARTY_DIR}/lib/libgtest.a"
      INTERFACE_INCLUDE_DIRECTORIES "${GTEST_3RDPARTY_DIR}/include"
      INTERFACE_LINK_LIBRARIES "Threads::Threads"
    )
  endif()
  if(NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main STATIC IMPORTED GLOBAL)
    set_target_properties(GTest::gtest_main PROPERTIES
      IMPORTED_LOCATION "${GTEST_3RDPARTY_DIR}/lib/libgtest_main.a"
      INTERFACE_INCLUDE_DIRECTORIES "${GTEST_3RDPARTY_DIR}/include"
      INTERFACE_LINK_LIBRARIES "GTest::gtest"
    )
  endif()
  if(NOT TARGET gtest)
    add_library(gtest ALIAS GTest::gtest)
  endif()
  if(NOT TARGET gtest_main)
    add_library(gtest_main ALIAS GTest::gtest_main)
  endif()
else()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  # 防止覆盖父项目的编译器/运行时设置
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(googletest)

  # 自动归档构建出的 libgtest.a / libgtest_main.a 和头文件到 3rdparty/googletest
  add_custom_target(archive_googletest_to_3rdparty ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GTEST_3RDPARTY_DIR}/lib" "${GTEST_3RDPARTY_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:gtest>" "${GTEST_3RDPARTY_DIR}/lib/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:gtest_main>" "${GTEST_3RDPARTY_DIR}/lib/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${googletest_SOURCE_DIR}/googletest/include/gtest" "${GTEST_3RDPARTY_DIR}/include/gtest"
    DEPENDS gtest gtest_main
    COMMENT "[3rdparty] Archiving GoogleTest static libraries to ${GTEST_3RDPARTY_DIR}"
  )
endif()

include(GoogleTest)
