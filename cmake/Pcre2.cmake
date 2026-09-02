# cmake/Pcre2.cmake
# PCRE2 智能缓存与预编译复用配置

set(PCRE2_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty/pcre2")
option(PCRE2_FORCE_REBUILD "Force rebuilding pcre2 even if cached in 3rdparty" OFF)
include(cmake/ThirdPartyCacheMetadata.cmake)
edgeflow_prepare_third_party_cache(
  NAME pcre2
  VERSION 10.47
  SOURCE_SHA256 c08ae2388ef333e8403e670ad70c0a11f1eed021fd88308d7e02f596fcd9dc16
  CACHE_DIR "${PCRE2_3RDPARTY_DIR}"
  KIND STATIC
  ABI_OPTIONS PCRE2_BUILD_PCRE2_8=ON PCRE2_STATIC_PIC=ON
  OUT_VALID _PCRE2_CACHE_VALID
  OUT_MARKER _PCRE2_CACHE_MARKER)

if(_PCRE2_CACHE_VALID AND
   EXISTS "${PCRE2_3RDPARTY_DIR}/lib/libpcre2-8.a" AND
   EXISTS "${PCRE2_3RDPARTY_DIR}/include/pcre2.h" AND
   NOT PCRE2_FORCE_REBUILD)
  message(STATUS "[3rdparty] Using cached PCRE2 from ${PCRE2_3RDPARTY_DIR}")

  if(NOT TARGET edgeflow_pcre2)
    add_library(edgeflow_pcre2 INTERFACE)
    target_include_directories(
        edgeflow_pcre2 SYSTEM INTERFACE "${PCRE2_3RDPARTY_DIR}/include")
    target_compile_definitions(edgeflow_pcre2 INTERFACE PCRE2_STATIC)
    if(APPLE)
      target_link_directories(edgeflow_pcre2 INTERFACE "${PCRE2_3RDPARTY_DIR}/lib")
      target_link_libraries(edgeflow_pcre2 INTERFACE "-Wl,-hidden-lpcre2-8")
    else()
      target_link_libraries(edgeflow_pcre2 INTERFACE "${PCRE2_3RDPARTY_DIR}/lib/libpcre2-8.a")
      if(UNIX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(
            edgeflow_pcre2 INTERFACE "LINKER:--exclude-libs,libpcre2-8.a")
      endif()
    endif()
  endif()
else()
  include(FetchContent)

  # TextRuleMatchNode uses PCRE2 for Unicode-aware lookbehind and named captures.
  # Keep the dependency private, source-built, pinned, and checksum-verified.
  set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "Build PCRE2 8-bit library" FORCE)
  set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "Disable PCRE2 16-bit library" FORCE)
  set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "Disable PCRE2 32-bit library" FORCE)
  set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "Disable pcre2grep" FORCE)
  set(PCRE2_BUILD_TESTS OFF CACHE BOOL "Disable PCRE2 upstream tests" FORCE)
  set(PCRE2_SHOW_REPORT OFF CACHE BOOL "Disable PCRE2 configuration report" FORCE)
  set(PCRE2_STATIC_PIC ON CACHE BOOL "Build position-independent PCRE2" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static third-party libraries" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable shared third-party libraries" FORCE)

  FetchContent_Declare(
      pcre2
      URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.gz
      URL_HASH SHA256=c08ae2388ef333e8403e670ad70c0a11f1eed021fd88308d7e02f596fcd9dc16
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  FetchContent_MakeAvailable(pcre2)

  # PCRE2 is an implementation detail of alg_sdk. Do not leak its C symbols from
  # the public shared-library surface when statically linking on ELF/Mach-O.
  if(TARGET pcre2-8-static AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pcre2-8-static PRIVATE -fvisibility=hidden)
  endif()

  if(NOT TARGET edgeflow_pcre2)
    add_library(edgeflow_pcre2 INTERFACE)
    if(APPLE)
      # Apple ld can mark every symbol pulled from a static archive as private.
      # Link the archive by name so -hidden-l applies to the complete PCRE2 object
      # set instead of exporting it through company_alg_sdk.dylib.
      add_dependencies(edgeflow_pcre2 pcre2-8-static)
      target_include_directories(
          edgeflow_pcre2 SYSTEM INTERFACE "${pcre2_BINARY_DIR}/interface")
      target_compile_definitions(edgeflow_pcre2 INTERFACE PCRE2_STATIC)
      target_link_directories(edgeflow_pcre2 INTERFACE "${pcre2_BINARY_DIR}")
      target_link_libraries(edgeflow_pcre2 INTERFACE "-Wl,-hidden-lpcre2-8")
    else()
      target_link_libraries(edgeflow_pcre2 INTERFACE pcre2-8)
      if(UNIX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(
            edgeflow_pcre2 INTERFACE "LINKER:--exclude-libs,libpcre2-8.a")
      endif()
    endif()
  endif()

  # 自动归档 PCRE2 静态库与头文件到 3rdparty/pcre2
  add_custom_target(archive_pcre2_to_3rdparty ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PCRE2_3RDPARTY_DIR}/lib" "${PCRE2_3RDPARTY_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pcre2-8-static>" "${PCRE2_3RDPARTY_DIR}/lib/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pcre2_BINARY_DIR}/interface/pcre2.h" "${PCRE2_3RDPARTY_DIR}/include/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_PCRE2_CACHE_MARKER}" "${PCRE2_3RDPARTY_DIR}/.edgeflow-cache-fingerprint"
    DEPENDS pcre2-8-static
    COMMENT "[3rdparty] Archiving PCRE2 static library to ${PCRE2_3RDPARTY_DIR}"
  )
endif()
