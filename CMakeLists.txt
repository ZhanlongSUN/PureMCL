cmake_minimum_required(VERSION 3.22)
project(PureMCL VERSION 0.1.0 LANGUAGES C RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  message(FATAL_ERROR "PureMCL requires MSVC targeting Windows x64.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(vendor "${CMAKE_CURRENT_BINARY_DIR}/vendor")
file(MAKE_DIRECTORY "${vendor}/miniz" "${vendor}/yyjson")
foreach(package IN ITEMS miniz-3.1.2 yyjson-0.13.0)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${package}.zip")
endforeach()
file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/miniz-3.1.2.zip"
     DESTINATION "${vendor}/miniz" PATTERNS miniz.c miniz.h LICENSE)
file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/yyjson-0.13.0.zip"
     DESTINATION "${vendor}/yyjson" PATTERNS "*/src/yyjson.c" "*/src/yyjson.h" "*/LICENSE")
add_library(nmc_vendor STATIC "${vendor}/miniz/miniz.c" "${vendor}/yyjson/yyjson-0.13.0/src/yyjson.c")
target_include_directories(nmc_vendor SYSTEM PUBLIC "${vendor}/miniz" "${vendor}/yyjson/yyjson-0.13.0/src")
target_compile_definitions(nmc_vendor PRIVATE _CRT_SECURE_NO_WARNINGS)

set(nmc_core_sources
  src/platform/win32/files.c src/platform/win32/process.c
  src/storage/integrity.c src/storage/config.c
  src/network/http.c src/network/download.c
  src/core/version.c src/core/runtime.c src/core/arguments.c src/core/launcher.c
  src/auth/account.c)
add_library(nmc_core STATIC ${nmc_core_sources})
target_include_directories(nmc_core PUBLIC include PRIVATE src)
target_compile_definitions(nmc_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(nmc_core PRIVATE /W4 /utf-8 /sdl)
target_compile_options(nmc_core PUBLIC $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
target_link_libraries(nmc_core PUBLIC nmc_vendor PRIVATE bcrypt winhttp crypt32 shell32 ole32 advapi32 uuid)

set(nmc_ui_sources src/main.c src/ui/window.c src/ui/renderer.c)
add_executable(PureMCL WIN32 ${nmc_ui_sources} resources/app.rc)
target_include_directories(PureMCL PRIVATE src)
target_compile_options(PureMCL PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4> $<$<COMPILE_LANGUAGE:C>:/utf-8> $<$<COMPILE_LANGUAGE:C>:/sdl>)
target_link_libraries(PureMCL PRIVATE nmc_core d2d1 dwrite windowscodecs dwmapi comctl32 gdi32 user32 uxtheme ole32 uuid)
target_link_options(PureMCL PRIVATE /MANIFEST:NO)
set_source_files_properties(resources/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/resources/mark.png")
# Track application headers even when Ninja cannot decode localized /showIncludes.
set_property(SOURCE ${nmc_core_sources} ${nmc_ui_sources} tests/test_core.c tests/test_graphics.c
  APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/nmc.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/internal.h")
set_property(SOURCE ${nmc_ui_sources} tests/test_graphics.c APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ui.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/graphics_abi.h")
set_property(SOURCE src/auth/account.c APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/client_id.h")
install(TARGETS PureMCL RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(FILES "${vendor}/miniz/LICENSE" DESTINATION licenses RENAME miniz.txt)
install(FILES "${vendor}/yyjson/yyjson-0.13.0/LICENSE" DESTINATION licenses RENAME yyjson.txt)

include(CTest)
if(BUILD_TESTING)
  add_executable(nmc_tests tests/test_core.c)
  target_include_directories(nmc_tests PRIVATE src)
  target_compile_options(nmc_tests PRIVATE /W4 /utf-8)
  target_link_libraries(nmc_tests PRIVATE nmc_core)
  add_test(NAME nmc_core COMMAND nmc_tests)
  add_executable(nmc_graphics_tests tests/test_graphics.c src/ui/renderer.c resources/app.rc)
  target_include_directories(nmc_graphics_tests PRIVATE src)
  target_compile_options(nmc_graphics_tests PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4> $<$<COMPILE_LANGUAGE:C>:/utf-8>)
  target_link_libraries(nmc_graphics_tests PRIVATE nmc_core d2d1 dwrite windowscodecs gdi32 user32 ole32 uuid)
  target_link_options(nmc_graphics_tests PRIVATE /MANIFEST:NO)
  add_test(NAME nmc_graphics COMMAND nmc_graphics_tests)
endif()

