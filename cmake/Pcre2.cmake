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
