# PES6PORT — Android native library (libpes6.so) for a NativeActivity.
# Configure with the NDK toolchain; tools/android_build.sh does it all:
#   cmake -S runtime/android -B runtime/android/build \
#         -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake_minimum_required(VERSION 3.16)
project(pes6port_android C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PES6_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PSPRECOMP "${PES6_ROOT}/third_party/psprecomp")
set(GEN       "${PES6_ROOT}/build/gen")
set(GEN_OVL   "${PES6_ROOT}/build/gen_ovl")
set(COMMON    "${PES6_ROOT}/runtime/common")
set(GLUE      "${ANDROID_NDK}/sources/android/native_app_glue")

if(NOT EXISTS "${GEN}/pes6_funcs.c" OR NOT EXISTS "${GEN_OVL}/overlays_registry.c")
    message(FATAL_ERROR "generate the recompiled sources first (see AGENTS.md, quick commands)")
endif()
file(GLOB OVL_SOURCES CONFIGURE_DEPENDS "${GEN_OVL}/ovl_*_funcs_part*.c")

# Same flags as the desktop host: the generated code is compiled unoptimised
# (it is ~150 MB of C) but with sibling calls, which its tail transfers need.
# PES6_GEN_OPT: optimisation of the generated code. -O1 halves the CPU time
# of -O0 with identical frames (docs/18) and builds in ~3 min; -fwrapv and
# -fno-strict-aliasing keep the compiler from exploiting UB that the MIPS
# semantics do not have.
set(PES6_GEN_OPT "-O1" CACHE STRING "optimisation level of the generated code")
add_compile_options(-w ${PES6_GEN_OPT} -foptimize-sibling-calls -fwrapv -fno-strict-aliasing)
# PSP_MEM_FAST (docs/18 §6, docs/32): guest memory as one host reservation, accessors inlined
# into the generated code (mask and add, no call). On the desktop since iteration 35.
add_compile_definitions(PSP_MEM_FAST)

add_library(pes6 SHARED
    main.c
    "${PES6_ROOT}/runtime/desktop/overlays.c"
    "${PES6_ROOT}/runtime/desktop/input.c"
    "${PES6_ROOT}/runtime/desktop/bot.c"
    "${COMMON}/ge.c" "${COMMON}/mpeg.c" "${COMMON}/atrac.c" "${COMMON}/h264.c"
    "${COMMON}/version.c" "${COMMON}/afs_mods.c" "${COMMON}/launcher.c"
    "${COMMON}/at3.c" "${COMMON}/audio.c" "${COMMON}/commentary.c" "${COMMON}/widescreen.c" "${COMMON}/net.c" "${COMMON}/osk.c" "${COMMON}/settings.c"
    "${COMMON}/tuning.c" "${COMMON}/rules.c" "${COMMON}/flow.c" "${COMMON}/faces.c" "${COMMON}/iso_eboot.c"
    # the EBOOT of the player's ISO decrypted on the phone (docs/35): pspdecrypt, GPL-3
    "${PES6_ROOT}/third_party/pspdecrypt/PrxDecrypter.cpp" "${PES6_ROOT}/third_party/pspdecrypt/pspdecrypt_shim.cpp"
    "${PES6_ROOT}/third_party/pspdecrypt/libkirk/kirk_engine.c" "${PES6_ROOT}/third_party/pspdecrypt/libkirk/AES.c"
    "${PES6_ROOT}/third_party/pspdecrypt/libkirk/SHA1.c" "${PES6_ROOT}/third_party/pspdecrypt/libkirk/bn.c"
    "${PES6_ROOT}/third_party/pspdecrypt/libkirk/ec.c"
    "${GLUE}/android_native_app_glue.c"
    "${GEN}/pes6_funcs.c" "${GEN}/pes6_imports.c" "${GEN_OVL}/overlays_registry.c"
    ${OVL_SOURCES}
    # The toolkit runtime, compiled in directly (its own CMake also builds
    # host tools and tests, which make no sense for the device).
    "${PSPRECOMP}/src/cpu.c" "${PSPRECOMP}/src/mem.c" "${PSPRECOMP}/src/dispatch.c"
    "${PSPRECOMP}/src/ctors.c" "${PSPRECOMP}/src/render.c" "${PSPRECOMP}/src/vfpu.c"
    "${PSPRECOMP}/src/hle/hle.c" "${PSPRECOMP}/src/hle/sysmem.c" "${PSPRECOMP}/src/hle/threadman.c"
    "${PSPRECOMP}/src/hle/display.c" "${PSPRECOMP}/src/hle/ge.c" "${PSPRECOMP}/src/hle/sascore.c"
    "${PSPRECOMP}/src/hle/iofilemgr.c" "${PSPRECOMP}/src/hle/misc.c"
)
target_include_directories(pes6 PRIVATE "${GEN}" "${GEN_OVL}" "${PSPRECOMP}/include" "${COMMON}"
    "${PES6_ROOT}/runtime/desktop" "${GLUE}")
target_compile_definitions(pes6 PRIVATE PES6_HAVE_GL=1)

# The EBOOT this build was recompiled from (tools/android_build.sh, from build/EBOOT.elf): a
# decrypted ISO must give exactly it (iso_eboot.c). 0 = no check.
set(PES6_EBOOT_SIZE "0" CACHE STRING "size of build/EBOOT.elf")
set(PES6_EBOOT_FNV "0" CACHE STRING "FNV-1a 64 of build/EBOOT.elf (0x...ull)")
set_source_files_properties("${COMMON}/iso_eboot.c" PROPERTIES COMPILE_DEFINITIONS
    "PES6_EBOOT_SIZE=${PES6_EBOOT_SIZE};PES6_EBOOT_FNV=${PES6_EBOOT_FNV}")
set_source_files_properties("${PES6_ROOT}/third_party/pspdecrypt/PrxDecrypter.cpp"
    "${PES6_ROOT}/third_party/pspdecrypt/pspdecrypt_shim.cpp" PROPERTIES COMPILE_OPTIONS "-std=c++17;-O2")

# The product (runtime/common/product.h, docs/29): the one game version the app plays.
# Empty = the multi-version port of docs/17 (the original plus every mod).
set(PES6_PRODUCT_VERSION "pech2026" CACHE STRING "mods/<id> the app plays; empty for every version")
# Only the files that read it: the generated code does not recompile when it changes.
set_source_files_properties(main.c "${COMMON}/version.c" "${COMMON}/launcher.c"
    PROPERTIES COMPILE_DEFINITIONS "PES6_PRODUCT_VERSION=\"${PES6_PRODUCT_VERSION}\"")

# Movie and music decoding: the static FFmpeg built by tools/android_ffmpeg.sh.
# Without it the intro is black and there is no music.
set(FFMPEG "${PES6_ROOT}/vendor/ffmpeg-android")
if(EXISTS "${FFMPEG}/lib/libavcodec.a")
    target_include_directories(pes6 PRIVATE "${FFMPEG}/include")
    target_link_libraries(pes6 PRIVATE "${FFMPEG}/lib/libavcodec.a" "${FFMPEG}/lib/libswscale.a"
        "${FFMPEG}/lib/libavutil.a")
    target_compile_definitions(pes6 PRIVATE PES6_HAVE_FFMPEG=1)
    # FFmpeg's AArch64 assembly refers to its own tables non-PIC; binding the
    # library's symbols to itself lets it link into a shared object.
    target_link_options(pes6 PRIVATE -Wl,-Bsymbolic)
    message(STATUS "pes6: FFmpeg found, movie and music decoding enabled")
else()
    message(STATUS "pes6: no vendor/ffmpeg-android (run tools/android_ffmpeg.sh): no movie, no music")
endif()
# The hand-written runtime is what runs per pixel and per instruction helper.
set_source_files_properties(
    "${COMMON}/ge.c" "${COMMON}/audio.c" "${COMMON}/commentary.c" "${PSPRECOMP}/src/vfpu.c" "${PSPRECOMP}/src/mem.c"
    "${PSPRECOMP}/src/dispatch.c" "${PSPRECOMP}/src/hle/sascore.c"
    PROPERTIES COMPILE_OPTIONS "-O2;-g")   # -g: line numbers for simpleperf; stripped from the APK
target_link_libraries(pes6 PRIVATE android log EGL GLESv3 aaudio m)
# NativeActivity looks this symbol up; nothing in the library references it.
target_link_options(pes6 PRIVATE -u ANativeActivity_onCreate)
