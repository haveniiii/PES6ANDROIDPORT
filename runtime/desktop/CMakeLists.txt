# PES6PORT — desktop bring-up host.
#
# Builds the recompiled we10psp module (generated C) against the vendored
# psprecomp runtime. The generated files live in build/gen (git-ignored).
cmake_minimum_required(VERSION 3.16)
project(pes6port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PES6_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PSPRECOMP  "${PES6_ROOT}/third_party/psprecomp")
set(GEN        "${PES6_ROOT}/build/gen")

if(NOT EXISTS "${GEN}/pes6_funcs.c")
    message(FATAL_ERROR "missing ${GEN}/pes6_funcs.c — run: "
        "allegrexrecomp emit build/EBOOT.elf build/gen pes6")
endif()

# The generated C is ~1M lines; warnings off and low optimisation (it is
# machine-generated and huge, and -O2 blows up compile time/memory).
#
# -foptimize-sibling-calls matters for correctness, not speed: a loop that
# discovery split across contiguous functions becomes mutually-recursive C
# calls, and without tail-call elimination that grows the host stack until it
# overflows. Sibling-call optimisation turns each tail call into a jump, which
# restores the loop's real (constant-stack) behaviour.
# PES6_GEN_OPT: optimisation of the generated code. -O1 halves the CPU time
# of -O0 with identical frames (docs/18) and builds in ~3 min; -fwrapv and
# -fno-strict-aliasing keep the compiler from exploiting UB that the MIPS
# semantics do not have.
set(PES6_GEN_OPT "-O1" CACHE STRING "optimisation level of the generated code")
add_compile_options(-w ${PES6_GEN_OPT} -foptimize-sibling-calls -fwrapv -fno-strict-aliasing)

# PSP_MEM_FAST (docs/18 §6): guest memory as one host reservation, accessors inlined into the
# generated code. Every translation unit, toolkit included, must agree, hence a global
# definition before the toolkit's directory. The trace build keeps the checked accessors.
option(PSP_MEM_FAST "guest memory accesses as inline pointer arithmetic" ON)
if(PSP_MEM_FAST)
    add_compile_definitions(PSP_MEM_FAST)
endif()
add_subdirectory("${PSPRECOMP}" psprecomp_build)

# Recompiled MWo3 overlays (tools/ovl_recomp.py): one translation unit each,
# plus the registry that maps an overlay id to its register function.
set(GEN_OVL "${PES6_ROOT}/build/gen_ovl")
if(NOT EXISTS "${GEN_OVL}/overlays_registry.c")
    message(FATAL_ERROR "missing ${GEN_OVL}/overlays_registry.c — run: tools/ovl_recomp.py")
endif()
file(GLOB OVL_SOURCES CONFIGURE_DEPENDS "${GEN_OVL}/ovl_*_funcs_part*.c")

# The generated code is its own object library, with its own flags and include
# paths: it takes ~15 minutes to compile (game.ovl alone is 94 MB of C), and as
# part of the host target every change to the host's options recompiled it.
add_library(pes6_gen OBJECT
    "${GEN}/pes6_funcs.c"
    "${GEN}/pes6_imports.c"
    ${OVL_SOURCES}
)
target_include_directories(pes6_gen PRIVATE "${GEN}" "${GEN_OVL}" "${PSPRECOMP}/include")

add_executable(pes6
    main.c
    overlays.c
    input.c
    bot.c
    ../common/ge.c
    ../common/mpeg.c
    ../common/atrac.c
    ../common/h264.c
    ../common/at3.c
    ../common/audio.c
    ../common/commentary.c
    ../common/widescreen.c
    ../common/tuning.c
    ../common/rules.c
    ../common/flow.c
    ../common/faces.c
    ../common/net.c
    ../common/osk.c
    ../common/settings.c
    ../common/lang.c
    ../common/version.c
    ../common/afs_mods.c
    ../common/patch.c
    ../common/pcof.c
    ../common/launcher.c
    "${GEN_OVL}/overlays_registry.c"
    $<TARGET_OBJECTS:pes6_gen>
)
target_include_directories(pes6 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(pes6 PRIVATE psprecomp z)

# The patches' importer on its own (docs/38 §9): pes6patch list|import|bank|delete.
add_executable(pes6patch patch_tool.c ../common/patch.c ../common/pcof.c ../common/lang.c)
target_include_directories(pes6patch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_compile_options(pes6patch PRIVATE -O1 -Wall -Wextra)
target_link_libraries(pes6patch PRIVATE z)

# Movie decoding through FFmpeg's libavcodec. Without it the intro stays black.
# The product (runtime/common/product.h): empty since 7/11 (docs/38), the original plus every patch;
# a mods/<id> here makes the app play only that one (the old "only the Chilean patch", docs/29).
set(PES6_PRODUCT_VERSION "" CACHE STRING "mods/<id> the app plays alone; empty for every patch")
target_compile_definitions(pes6 PRIVATE PES6_PRODUCT_VERSION="${PES6_PRODUCT_VERSION}")

find_path(AVCODEC_INCLUDE libavcodec/avcodec.h HINTS /opt/homebrew/include /usr/local/include)
find_library(AVCODEC_LIB avcodec HINTS /opt/homebrew/lib /usr/local/lib)
find_library(AVUTIL_LIB avutil HINTS /opt/homebrew/lib /usr/local/lib)
find_library(SWSCALE_LIB swscale HINTS /opt/homebrew/lib /usr/local/lib)
if(AVCODEC_INCLUDE AND AVCODEC_LIB AND AVUTIL_LIB AND SWSCALE_LIB)
    target_include_directories(pes6 PRIVATE "${AVCODEC_INCLUDE}")
    target_link_libraries(pes6 PRIVATE "${AVCODEC_LIB}" "${AVUTIL_LIB}" "${SWSCALE_LIB}")
    target_compile_definitions(pes6 PRIVATE PES6_HAVE_FFMPEG=1)
    message(STATUS "pes6: libavcodec found, movie decoding enabled")
else()
    message(STATUS "pes6: libavcodec not found, movies stay black")
endif()

# Window + real input (pes6 --window). Without SDL2 the host is headless only.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_sources(pes6 PRIVATE window.c)
    target_link_libraries(pes6 PRIVATE SDL2::SDL2)
    target_compile_definitions(pes6 PRIVATE PES6_HAVE_SDL=1)
    # GPU rasterization (pes6 --window --gpu): desktop OpenGL core profile.
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(pes6 PRIVATE OpenGL::GL)
        target_compile_definitions(pes6 PRIVATE PES6_HAVE_GL=1)
        message(STATUS "pes6: OpenGL found, --gpu enabled")
    endif()
    message(STATUS "pes6: SDL2 found, --window enabled")
else()
    message(STATUS "pes6: SDL2 not found, headless only")
endif()
# Hand-written hot code: the rasterizer runs per pixel, so it is optimised and
# keeps its warnings, unlike the generated sources.
set_source_files_properties(../common/ge.c ../common/mpeg.c ../common/atrac.c ../common/audio.c
    PROPERTIES COMPILE_OPTIONS "-O2;-Wall;-Wextra")

# Instrumented build: records function entries, interior marks and loop
# back-edges so a hang can be located. Toggle with -DPES6_TRACE=ON.
option(PES6_TRACE "enable psprecomp bring-up instrumentation" OFF)
if(PES6_TRACE)
    target_compile_definitions(pes6_gen PRIVATE PSPRECOMP_TRACE)
endif()
if(PES6_TRACE AND PSP_MEM_FAST)
    message(FATAL_ERROR "PES6_TRACE needs the checked memory accessors: configure with -DPSP_MEM_FAST=OFF")
endif()
