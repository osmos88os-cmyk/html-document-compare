cmake_minimum_required(VERSION 3.24)
project(HTMLCompare VERSION 0.2.0 LANGUAGES C CXX)
option(HTMLCOMPARE_LEGACY "Build the Windows 7 SP1 edition" OFF)
if(WIN32)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use MSVC x64 for Windows packages")
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
foreach(feature CATALOG DEBUG DOCBOOK FTP HTTP ICONV ICU LZMA MODULES PYTHON
                READLINE REGEXPS SCHEMAS SCHEMATRON TESTS VALID XINCLUDE XPTR ZLIB)
  set(LIBXML2_WITH_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libxml2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://codeload.github.com/GNOME/libxml2/tar.gz/refs/tags/v2.15.1
  URL_HASH SHA256=23b1ebd74cc562cd592cd2618c2bd88dc06fa0dfdbcd56e03cb26d6ff6b7e373)
FetchContent_Declare(nlohmann_json
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(utf8proc
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.10.0
  URL_HASH SHA256=6f4f1b639daa6dca9f80bc5db1233e9cbaa31a67790887106160b33ef743f136)
FetchContent_MakeAvailable(libxml2 nlohmann_json utf8proc)

add_library(compare_core STATIC
  native/src/text.cpp native/src/html.cpp native/src/diff.cpp native/src/review.cpp)
target_include_directories(compare_core PUBLIC native/src)
target_link_libraries(compare_core PRIVATE LibXml2::LibXml2 nlohmann_json::nlohmann_json utf8proc)
if(WIN32)
  target_link_libraries(compare_core PRIVATE bcrypt)
  target_compile_definitions(compare_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
elseif(NOT APPLE)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(compare_core PRIVATE OpenSSL::Crypto)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(compare_tests native/tests/compare_tests.cpp)
  target_link_libraries(compare_tests PRIVATE compare_core)
  add_test(NAME compare_core COMMAND compare_tests)
endif()

if(WIN32)
  if(HTMLCOMPARE_LEGACY AND NOT MSVC_TOOLSET_VERSION STREQUAL "143")
    message(FATAL_ERROR "Windows 7 build requires the MSVC v143 toolset")
  endif()
  set(target_winver 0x0A00)
  set(subsystem_version 10.00)
  if(HTMLCOMPARE_LEGACY)
    set(target_winver 0x0601)
    set(subsystem_version 6.01)
  endif()
  add_executable(HTMLCompare WIN32 native/src/win32.cpp native/src/sheet.cpp native/resources/app.rc)
  target_include_directories(HTMLCompare PRIVATE native/resources)
  target_compile_definitions(HTMLCompare PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    NOMINMAX _WIN32_WINNT=${target_winver} WINVER=${target_winver}
    HTMLCOMPARE_LEGACY=$<BOOL:${HTMLCOMPARE_LEGACY}>)
  target_link_libraries(HTMLCompare PRIVATE compare_core comctl32 comdlg32 d2d1 dwrite shell32)
  target_link_options(HTMLCompare PRIVATE "/SUBSYSTEM:WINDOWS,${subsystem_version}" "/MANIFEST:NO")
  install(TARGETS HTMLCompare RUNTIME DESTINATION .)
  install(FILES native/README.md native/THIRD_PARTY_NOTICES.md DESTINATION .)
  install(FILES "${libxml2_SOURCE_DIR}/Copyright" DESTINATION licenses RENAME libxml2.txt)
  install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json.txt)
  install(FILES "${utf8proc_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME utf8proc.txt)
  install(DIRECTORY examples/ DESTINATION examples)
endif()
