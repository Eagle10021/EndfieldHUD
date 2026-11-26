# Native Linux build of EndfieldHUD, compiled from the original sources in ../windows.
cmake_minimum_required(VERSION 3.25)
project(EndfieldHUDLinux LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Strict C++ keeps identifiers such as `linux` and `unix` from being predefined macros.
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/../windows")

# Sources that still need Windows headers; each needs a Linux implementation or a guard.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/windows-only-sources.txt" WINDOWS_ONLY REGEX "^[^#]")

file(GLOB_RECURSE UPSTREAM_SOURCES CONFIGURE_DEPENDS RELATIVE "${UPSTREAM}"
    "${UPSTREAM}/core/*.cpp" "${UPSTREAM}/modules/*.cpp" "${UPSTREAM}/native/*.cpp")
list(REMOVE_ITEM UPSTREAM_SOURCES ${WINDOWS_ONLY})
# Portable pieces of the Windows application layer
list(APPEND UPSTREAM_SOURCES app/utility_executor.cpp)
list(TRANSFORM UPSTREAM_SOURCES PREPEND "${UPSTREAM}/")

add_library(hud_upstream STATIC ${UPSTREAM_SOURCES})
target_include_directories(hud_upstream PUBLIC "${UPSTREAM}" "${UPSTREAM}/app" "${UPSTREAM}/core" "${UPSTREAM}/modules" "${UPSTREAM}/native")
# The same bundled JavaScript engine settings as the upstream Windows build.
set(QJS "${UPSTREAM}/third_party/quickjs-ng")
add_library(hud_quickjs STATIC "${QJS}/dtoa.c" "${QJS}/libregexp.c" "${QJS}/libunicode.c" "${QJS}/quickjs.c")
set_target_properties(hud_quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS ON)
target_include_directories(hud_quickjs PUBLIC "${QJS}")
target_compile_definitions(hud_quickjs PRIVATE _GNU_SOURCE __STDC_NO_ATOMICS__=1)
target_compile_options(hud_quickjs PRIVATE -funsigned-char -w)

find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(hud_upstream PUBLIC hud_quickjs ICU::i18n ICU::uc ZLIB::ZLIB SQLite::SQLite3 Threads::Threads m)

# std headers that the Windows toolchain includes transitively
target_compile_options(hud_upstream PRIVATE -include algorithm -w)

# Lists the symbols the Linux platform layer still has to provide:
#   cmake --build build --target missing-symbols
add_library(hud_link_check SHARED EXCLUDE_FROM_ALL empty.cpp)
target_link_libraries(hud_link_check PRIVATE -Wl,--whole-archive hud_upstream -Wl,--no-whole-archive)
target_link_options(hud_link_check PRIVATE -Wl,--no-undefined)
add_custom_target(missing-symbols DEPENDS hud_link_check)
