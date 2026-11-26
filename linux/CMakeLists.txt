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
list(APPEND UPSTREAM_SOURCES app/utility_executor.cpp app/source_watch_session.cpp)
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

# Draw validation and texture conversion, taken from the Direct3D renderer at build time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DRAW_LOGIC "${CMAKE_CURRENT_BINARY_DIR}/generated/renderer_logic.cpp")
add_custom_command(OUTPUT "${DRAW_LOGIC}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_renderer_logic.py"
            "${UPSTREAM}/native/renderer.cpp" "${DRAW_LOGIC}"
    DEPENDS tools/extract_renderer_logic.py "${UPSTREAM}/native/renderer.cpp" VERBATIM)
# The OpenGL renderer includes the generated logic, so both share one translation unit.
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
add_custom_target(renderer_logic DEPENDS "${DRAW_LOGIC}")
target_sources(hud_upstream PRIVATE platform/renderer_gl.cpp)
set_source_files_properties(platform/renderer_gl.cpp PROPERTIES OBJECT_DEPENDS "${DRAW_LOGIC}"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(hud_upstream renderer_logic)
target_link_libraries(hud_upstream PUBLIC PkgConfig::EPOXY)

# Text and layers: Cairo, Pango and the bundled fonts.
pkg_check_modules(TEXT REQUIRED IMPORTED_TARGET pangocairo pangoft2 cairo fontconfig harfbuzz gdk-pixbuf-2.0)
set(RELIEF "${CMAKE_CURRENT_BINARY_DIR}/generated/raster_relief.inc")
add_custom_command(OUTPUT "${RELIEF}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_renderer_logic.py" --raster-relief
            "${UPSTREAM}/native/layer_raster.cpp" "${RELIEF}"
    DEPENDS tools/extract_renderer_logic.py "${UPSTREAM}/native/layer_raster.cpp" VERBATIM)
add_custom_target(raster_relief DEPENDS "${RELIEF}")
target_sources(hud_upstream PRIVATE platform/layer_raster_linux.cpp)
set_source_files_properties(platform/layer_raster_linux.cpp PROPERTIES OBJECT_DEPENDS "${RELIEF}"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(hud_upstream raster_relief)
target_link_libraries(hud_upstream PUBLIC PkgConfig::TEXT)

# The rasterizer loads <executable directory>/fonts, as on Windows.
file(CREATE_LINK "${UPSTREAM}/../Resources/Fonts" "${CMAKE_CURRENT_BINARY_DIR}/fonts" SYMBOLIC)

# The original material pass: Direct3D bytecode translated by libvkd3d-shader.
pkg_check_modules(VKD3D REQUIRED IMPORTED_TARGET libvkd3d-shader)
set(SOURCE_SCENE "${CMAKE_CURRENT_BINARY_DIR}/generated/source_scene_linux.cpp")
add_custom_command(OUTPUT "${SOURCE_SCENE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_renderer_logic.py" --source-scene
            "${UPSTREAM}/native/source_scene.cpp" "${SOURCE_SCENE}"
    DEPENDS tools/extract_renderer_logic.py "${UPSTREAM}/native/source_scene.cpp" VERBATIM)
get_target_property(PORTABLE_SOURCES hud_upstream SOURCES)
list(REMOVE_ITEM PORTABLE_SOURCES "${UPSTREAM}/native/source_scene.cpp")
set_property(TARGET hud_upstream PROPERTY SOURCES ${PORTABLE_SOURCES})
target_sources(hud_upstream PRIVATE "${SOURCE_SCENE}" platform/source_graphics_gl.cpp)

# The navigation captions and icons are portable code behind a Windows guard.
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APPEARANCE "${GENERATED}/watch_appearance_linux.cpp")
set(APPEARANCE_HEADER "${GENERATED}/override/native/watch_appearance.hpp")
add_custom_command(OUTPUT "${APPEARANCE}" "${APPEARANCE_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED}/override/native"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_renderer_logic.py" --unguard
            "${UPSTREAM}/native/watch_appearance.cpp" "${APPEARANCE}"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_renderer_logic.py" --unguard
            "${UPSTREAM}/native/watch_appearance.hpp" "${APPEARANCE_HEADER}"
    DEPENDS tools/extract_renderer_logic.py "${UPSTREAM}/native/watch_appearance.cpp" "${UPSTREAM}/native/watch_appearance.hpp" VERBATIM)
get_target_property(PORTABLE_SOURCES hud_upstream SOURCES)
list(REMOVE_ITEM PORTABLE_SOURCES "${UPSTREAM}/native/watch_appearance.cpp")
set_property(TARGET hud_upstream PROPERTY SOURCES ${PORTABLE_SOURCES})
target_sources(hud_upstream PRIVATE "${APPEARANCE}")
target_include_directories(hud_upstream BEFORE PUBLIC "${GENERATED}/override")
target_link_libraries(hud_upstream PUBLIC PkgConfig::VKD3D)
target_include_directories(hud_upstream PRIVATE platform)

include(CTest)
if(BUILD_TESTING)
    add_executable(renderer_test tests/renderer_test.cpp)
    target_compile_definitions(renderer_test PRIVATE SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/platform/shaders")
    target_link_libraries(renderer_test PRIVATE hud_upstream)
    add_test(NAME renderer COMMAND renderer_test)
endif()

# Shaders are checked at build time: every variant of the scene pass must compile.
find_program(GLSLANG glslangValidator REQUIRED)
set(SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/platform/shaders")
set(SHADER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders.checked")
set(SHADER_COMMANDS COMMAND "${GLSLANG}" -G "-I${SHADERS}" -o "${CMAKE_CURRENT_BINARY_DIR}/generated/scene.vert.spv" "${SHADERS}/scene.vert")
foreach(variant "" "-DENCODED_INPUT" "-DENCODED_OUTPUT" "-DENCODED_INPUT;-DENCODED_OUTPUT" "-DALPHA_INPUT" "-DALPHA_INPUT;-DENCODED_OUTPUT")
    list(APPEND SHADER_COMMANDS COMMAND "${GLSLANG}" -G "-I${SHADERS}" ${variant} -o "${CMAKE_CURRENT_BINARY_DIR}/generated/scene.frag.spv" "${SHADERS}/scene.frag")
endforeach()
add_custom_command(OUTPUT "${SHADER_STAMP}" ${SHADER_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${SHADER_STAMP}"
    DEPENDS "${SHADERS}/scene.glsl" "${SHADERS}/scene.vert" "${SHADERS}/scene.frag" COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(shaders ALL DEPENDS "${SHADER_STAMP}")

# Lists the symbols the Linux platform layer still has to provide:
#   cmake --build build --target missing-symbols
add_library(hud_link_check SHARED EXCLUDE_FROM_ALL empty.cpp)
target_link_libraries(hud_link_check PRIVATE -Wl,--whole-archive hud_upstream -Wl,--no-whole-archive)
target_link_options(hud_link_check PRIVATE -Wl,--no-undefined)
add_custom_target(missing-symbols DEPENDS hud_link_check)

# Test programs shipped with the Windows sources, built unmodified.
function(upstream_test name)
    add_executable(upstream_${name} "${UPSTREAM}/tests/${name}_tests.cpp" ${ARGN})
    target_include_directories(upstream_${name} PRIVATE tests/compat)
    target_link_libraries(upstream_${name} PRIVATE hud_upstream)
    add_test(NAME upstream_${name} COMMAND upstream_${name})
    set_tests_properties(upstream_${name} PROPERTIES TIMEOUT 120)
endfunction()
if(BUILD_TESTING)
    # Omitted until editor text layouts are retained: the text metadata budget checks.
    set(RASTER_TEST "${CMAKE_CURRENT_BINARY_DIR}/generated/layer_raster_tests.cpp")
    add_custom_command(OUTPUT "${RASTER_TEST}"
        COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/filter_upstream_test.py"
                "${UPSTREAM}/tests/layer_raster_tests.cpp" "${RASTER_TEST}" "// Text metadata:" "raster.clear();}"
        DEPENDS tools/filter_upstream_test.py "${UPSTREAM}/tests/layer_raster_tests.cpp" VERBATIM)
    add_executable(upstream_layer_raster "${RASTER_TEST}")
    target_include_directories(upstream_layer_raster PRIVATE tests/compat)
    target_link_libraries(upstream_layer_raster PRIVATE hud_upstream)
    add_test(NAME upstream_layer_raster COMMAND upstream_layer_raster)
    set_tests_properties(upstream_layer_raster PROPERTIES TIMEOUT 120)
    upstream_test(layer_mask)
    upstream_test(layer_source_text)
    upstream_test(layer_image_source)
endif()

# Offscreen preview of the dial from an official release's resources.
add_library(hud_shell STATIC platform/hud_shell.cpp)
target_include_directories(hud_shell PUBLIC platform)
target_link_libraries(hud_shell PUBLIC hud_upstream)
add_executable(hud_preview tools/hud_preview.cpp)
target_link_libraries(hud_preview PRIVATE hud_shell)

# The overlay application: Qt for the Wayland window, LayerShellQt for the overlay layer,
# KDE's global shortcuts and background blur.
option(BUILD_APP "Build the KDE Plasma overlay application" ON)
if(BUILD_APP)
    find_package(Qt6 REQUIRED COMPONENTS Gui Widgets DBus OpenGL)
    find_package(LayerShellQt REQUIRED)
    find_package(KF6GlobalAccel REQUIRED)
    find_package(KF6WindowSystem REQUIRED)
    add_executable(endfieldhud app/main.cpp platform/presenter_gl.cpp)
    set_target_properties(endfieldhud PROPERTIES AUTOMOC ON)
    target_compile_definitions(endfieldhud PRIVATE QT_NO_KEYWORDS SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/platform/shaders")
    target_link_libraries(endfieldhud PRIVATE hud_shell Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::OpenGL LayerShellQt::Interface KF6::GlobalAccel KF6::WindowSystem)
endif()
