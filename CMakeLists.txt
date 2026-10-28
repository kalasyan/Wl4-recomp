# Native build: Linux, Android (from android/, through Gradle) and Windows (headless only).
# The GBA ROM is still built with make. See docs/PORT.md.
cmake_minimum_required(VERSION 3.22)
project(wl4_recomp C ASM)

option(WL4_CAST_WARNINGS "Show the pointer/integer cast warnings (the GbaPtr worklist)" OFF)
option(WL4_ASAN "Build with AddressSanitizer" OFF)

if(WL4_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

set(WL4_IMAGE_BASE "0x10000000" CACHE STRING "Android: preferred load address of libgame.so (below 4 GiB)")
set(WL4_SDL3_TAG "release-3.4.18" CACHE STRING "SDL3 version fetched when no SDL3 is installed")
set(WL4_IMGUI_TAG "v1.92.9b" CACHE STRING "Dear ImGui version (options menu)")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm64-v8a)$")
    set(WL4_ARM64 ON)
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The port requires Clang (-fms-volatile keeps DMA ordering)")
endif()

if(WIN32)
    set(WL4_OS_SOURCE port/src/os_win32.c)
    set(WL4_SAVE_TEST_SOURCE port/src/save_test.c)
elseif(ANDROID)
    # Everything goes in libgame.so, which the launcher (port/frontend/android_launcher.c)
    # loads below 4 GiB: code reaches the game's data, absolute symbols at GBA addresses,
    # PC-relative.
    set(WL4_OS_SOURCE $<IF:$<BOOL:${WL4_ARM64}>,port/src/os_linux_arm64.c,port/src/os_linux.c>)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(WL4_SAVE_TEST_SOURCE port/src/save_test_linux.c)
    if(WL4_ARM64)
        set(WL4_OS_SOURCE port/src/os_linux_arm64.c)
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(WL4_OS_SOURCE port/src/os_linux.c)
    else()
        message(FATAL_ERROR "Linux builds are for x86-64 and AArch64")
    endif()
    # Absolute GBA symbols require ET_EXEC. Place code and the randomized brk heap
    # above all GBA mappings so startup allocations cannot collide with the ROM.
    add_compile_options(-fno-pie)
    if(WL4_ASAN)
        # Linux ASan otherwise moves address-taken locals to a high fake stack,
        # which cannot be stored in the game's 32-bit DMA addresses.
        add_compile_options(-fsanitize-address-use-after-return=never)
    endif()
    set(WL4_EXE_LINK_OPTIONS -no-pie -Wl,-z,noexecstack -Wl,-Ttext-segment=0x40000000)
else()
    message(FATAL_ERROR "Supported targets are Linux (x86-64, AArch64), Android and Windows (headless)")
endif()

if(WL4_ARM64)
    set(WL4_ACCESS_SOURCES port/src/arm64_access.c)
else()
    set(WL4_ACCESS_SOURCES port/src/x86_access.c)
endif()

# Game code: everything in src/ except the ROM data in src/data/, which the port reads from
# the user's ROM. The function pointer tables there hold native addresses, so they are built.
file(GLOB_RECURSE WL4_GAME_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.c)
list(FILTER WL4_GAME_SOURCES EXCLUDE REGEX "/src/data/")
list(APPEND WL4_GAME_SOURCES
    ${PROJECT_SOURCE_DIR}/src/data/color_fading_pointers.c
    ${PROJECT_SOURCE_DIR}/src/data/stage_end_pointers.c
    ${PROJECT_SOURCE_DIR}/src/data/tile_fn_pointers.c
)

file(GLOB WL4_LAYOUT_ASSERTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/port/generated/layout_asserts_*.c)

# How game code is compiled, shared by the game library and the layout checks.
add_library(wl4_game_flags INTERFACE)
target_include_directories(wl4_game_flags INTERFACE include port/include)
target_compile_definitions(wl4_game_flags INTERFACE PORTABLE VERSION_US)
target_compile_options(wl4_game_flags INTERFACE
    -std=gnu89
    # agbcc's semantics: unsigned char, no type-based aliasing, wrapping signed overflow,
    # and null pointers that may be dereferenced (GBA address 0 is the BIOS).
    -funsigned-char -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    # GCC bitfield layout, as agbcc uses, instead of the Microsoft layout mingw defaults to.
    -mno-ms-bitfields
    # The game's data sits at its GBA addresses, which are aligned to 4 bytes at most. Clang
    # takes every global array of 16 bytes or more to be 16-byte aligned, and copies them
    # with aligned SSE loads, which fault. No SSE in game code, then (it has no float maths
    # beyond a few casts, which x87 does).
    $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:-mno-sse>
    # Game code starts DMA by writing the registers, often to copy a local variable it has just
    # set: `fill = 0; REG_DMA3SAD = (u32)&fill; ... REG_DMA3CNT = ...;`. Clang would drop or
    # delay the store to the local, which only the transfer reads. Acquire/release semantics
    # for volatile accesses keep every store before the register write that starts a DMA.
    -fms-volatile
    -Werror=return-type -Werror=implicit-function-declaration
    # Pointer type mismatches and int/pointer conversions are fixed by the GbaPtr conversion.
    -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion
)
if(NOT WL4_CAST_WARNINGS)
    target_compile_options(wl4_game_flags INTERFACE
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-void-pointer-to-int-cast -Wno-int-to-void-pointer-cast)
endif()

add_library(wl4_game STATIC ${WL4_GAME_SOURCES} ${WL4_LAYOUT_ASSERTS})
target_link_libraries(wl4_game PRIVATE wl4_game_flags)

# Layout checks for the structs that game sources define themselves. Each one includes its
# source file, so these objects are compiled but never linked.
file(GLOB WL4_LAYOUT_CHECKS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/port/generated/layout_checks/*.c)
add_library(wl4_layout_checks OBJECT ${WL4_LAYOUT_CHECKS})
target_link_libraries(wl4_layout_checks PRIVATE wl4_game_flags)
# The game library already reports these sources' warnings.
target_compile_options(wl4_layout_checks PRIVATE -w)

add_library(wl4_platform_stub STATIC port/platform/stub/platform_stub.c)

# Native replacements for game asm that need the game's headers. An object library, so that
# its definitions are always linked and replace the weak stubs in port/generated/gba_stubs.c.
add_library(wl4_game_native OBJECT port/src/m4a_native.c port/src/rumble_snapshot.c port/src/tile_wrap_test.c)
target_link_libraries(wl4_game_native PRIVATE wl4_game_flags)

if(NOT ANDROID)
    add_executable(wl4-rumble-event-test port/src/rumble_event_test.c port/src/rumble_snapshot.c)
    target_link_libraries(wl4-rumble-event-test PRIVATE wl4_game_flags)
    target_link_options(wl4-rumble-event-test PRIVATE ${WL4_EXE_LINK_OPTIONS})
endif()

# The port runtime: GBA memory, I/O, video, sound and BIOS. port/generated/gba_*.{c,s} come
# from port/tools/gen_symbols.py: GBA addresses for the game's data, the function dispatch
# table, and stubs for the functions that are still asm.
set(WL4_SYMBOLS ${PROJECT_SOURCE_DIR}/port/generated/gba_symbols.s)
if(ANDROID)
    # Hidden, so that they resolve at link time with no dynamic relocations.
    file(READ ${WL4_SYMBOLS} WL4_SYMBOLS_TEXT)
    string(REGEX REPLACE "\\.globl ([A-Za-z0-9_]+)" ".globl \\1\n    .hidden \\1" WL4_SYMBOLS_TEXT "${WL4_SYMBOLS_TEXT}")
    file(WRITE ${CMAKE_BINARY_DIR}/gba_symbols_hidden.s "${WL4_SYMBOLS_TEXT}")
    set(WL4_SYMBOLS ${CMAKE_BINARY_DIR}/gba_symbols_hidden.s)
endif()

add_library(wl4_runtime STATIC
    port/src/audio.c
    port/src/game_thread.c
    port/src/bios.c
    port/src/frame.c
    port/src/gba_functions.c
    port/src/gba_io.c
    port/src/gba_memory.c
    port/src/gba_runtime.c
    port/src/md5.c
    port/src/ppu.c
    port/src/rom_validate.c
    ${WL4_OS_SOURCE}
    ${WL4_ACCESS_SOURCES}
    ${WL4_SYMBOLS}
    port/generated/gba_functions.c
    port/generated/gba_stubs.c
    port/generated/lz77_sources.c
)
target_include_directories(wl4_runtime PUBLIC port/include PRIVATE port/third_party/minicoro)
# gba_functions.c declares every game function as void(void) to take its address.
set_source_files_properties(port/generated/gba_functions.c PROPERTIES COMPILE_OPTIONS -Wno-incompatible-library-redeclaration)
target_link_libraries(wl4_runtime PUBLIC wl4_game wl4_platform_stub)
if(ANDROID)
    target_link_libraries(wl4_runtime PUBLIC log)
endif()

# The game's native pieces and the runtime reference each other.
set(WL4_LINK $<TARGET_OBJECTS:wl4_game_native>)

# wl4_headless: runs the game without a window, for tests and comparisons with mGBA.
if(NOT ANDROID)
    add_executable(wl4_headless
        port/src/main.c
        port/src/bios_test.c
        port/src/input_script.c
        port/src/io_test.c
        port/src/rom.c
        port/src/save.c
        ${WL4_SAVE_TEST_SOURCE}
        port/src/arm64_access_test.c
        ${WL4_LINK}
    )
    target_include_directories(wl4_headless PRIVATE port/include port/third_party/minicoro)
    target_link_libraries(wl4_headless PRIVATE wl4_runtime wl4_game wl4_runtime)
    target_link_options(wl4_headless PRIVATE ${WL4_EXE_LINK_OPTIONS})
    if(NOT WIN32)
        target_sources(wl4_headless PRIVATE port/src/os_file_posix.c)
    endif()
endif()

# The frontend's core (save states, saves, rumble) with a host that has no window, for tests.
if(NOT ANDROID AND NOT WIN32)
    add_executable(wl4_core_test
        port/frontend/core.c
        port/frontend/rumble.c
        port/frontend/host_test.c
        port/src/input_script.c
        ${WL4_LINK})
    target_include_directories(wl4_core_test PRIVATE port/frontend port/include)
    target_compile_options(wl4_core_test PRIVATE -std=gnu11 -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(wl4_core_test PRIVATE wl4_runtime wl4_game wl4_runtime m)
    target_link_options(wl4_core_test PRIVATE ${WL4_EXE_LINK_OPTIONS})
endif()

# The game with its window, menu and touch controls (port/frontend): SDL3 and Dear ImGui.
option(WL4_FRONTEND "Build the SDL3 frontend" ON)
if(WL4_FRONTEND AND NOT WIN32)
    if(NOT ANDROID)
        find_package(SDL3 CONFIG QUIET)
    endif()
    include(FetchContent)
    if(NOT TARGET SDL3::SDL3)
        # No system SDL3 (always the case on Android): build it from source.
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        if(NOT ANDROID)
            # Linked in, so the executable runs anywhere (AppImage, Steam Deck).
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
        endif()
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG ${WL4_SDL3_TAG}
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    enable_language(CXX)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG ${WL4_IMGUI_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(wl4_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(wl4_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(wl4_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS
                                               IMGUI_DISABLE_DEBUG_TOOLS)
    target_link_libraries(wl4_imgui PUBLIC SDL3::SDL3)

    set(WL4_FRONTEND_SOURCES
        port/frontend/core.c
        port/frontend/host_sdl.c
        port/frontend/config.c
        port/frontend/pacing.c
        port/frontend/rumble.c
        port/frontend/menu.cpp
        ${WL4_LINK})
    if(ANDROID)
        # SDLActivity loads libmain.so and calls its SDL_main, which loads the game
        # (libgame.so) at a reserved address below 4 GiB and runs it.
        add_library(game SHARED ${WL4_FRONTEND_SOURCES})
        set(WL4_EXE game)
        add_library(main SHARED port/frontend/android_launcher.c)
        target_link_libraries(main PRIVATE dl log)
        target_compile_definitions(main PRIVATE WL4_IMAGE_BASE=${WL4_IMAGE_BASE})
        add_dependencies(main game)
    else()
        add_executable(wl4 ${WL4_FRONTEND_SOURCES})
        set(WL4_EXE wl4)
        target_link_options(wl4 PRIVATE ${WL4_EXE_LINK_OPTIONS})
    endif()
    target_include_directories(${WL4_EXE} PRIVATE port/frontend port/include)
    target_compile_options(${WL4_EXE} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11> -Wall -Wextra -Wno-unused-parameter)
    target_compile_features(${WL4_EXE} PRIVATE cxx_std_17)
    target_link_libraries(${WL4_EXE} PRIVATE wl4_runtime wl4_game wl4_runtime wl4_imgui SDL3::SDL3 m)
endif()
