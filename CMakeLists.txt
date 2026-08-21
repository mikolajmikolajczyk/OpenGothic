cmake_minimum_required(VERSION 3.16)

project(Gothic2Notr LANGUAGES C CXX)

# ⚠ THE ONE CHECK THAT CANNOT LIVE IN THE TOOLCHAIN FILE. orbis-compat's headers only work when its
# include directory is searched AHEAD of the SDK's - behind it they compile, do nothing and say
# nothing, and the build succeeds against pthread types that are the wrong size. Proving that needs
# a working compiler, which does not exist yet while a toolchain file is being read. Here it does.
if (PS4 AND COMMAND orbis_compat_verify)
	orbis_compat_verify()
endif ()

set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/opengothic)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/opengothic)
set(CMAKE_DEBUG_POSTFIX "")
set(CMAKE_RELWITHDEBINFO_POSTFIX "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_RPATH ON)

add_executable(${PROJECT_NAME})

# ---------------------------------------------------------------- PS4 (Orbis) target
#
# Configured with -DCMAKE_TOOLCHAIN_FILE=<orbis-compat>/cmake/ps4-openorbis.cmake, which sets
# PS4=TRUE and CMAKE_SYSTEM_NAME=Generic (so neither UNIX nor WIN32 is true below, and
# none of the -rdynamic / -lpthread -ldl / windows branches fire).
#
# Everything PS4-specific that is not in this file lives in one of two places, and neither is the
# Tempest fork any more:
#
#   ps4/            THIS TITLE's: the game-data discovery, the silent sound backend, the on-screen
#                   keyboard and the C++20 shim. See ps4/og_ps4_boot.h for the data-root rules.
#   orbis-compat/   THE PLATFORM's: the SDK corrections, the Vulkan C ABI (vkloader) and the console
#                   log channel (optional/, ps4-app). Shared with Mesa and the CTS.
#
# ⚠ NOTHING PS4-SPECIFIC IS TAKEN FROM lib/Tempest ANY MORE. vkloader moved to the overlay on
# 2026-08-20 and ps4-app with it; the engine only requires that a ps4-vkloader target exist by the
# time Engine/ is configured, which the add_subdirectory below satisfies.
if(PS4)
  set(OG_PS4_SRC "${CMAKE_CURRENT_SOURCE_DIR}/ps4")
  # Set by orbis-compat's cmake/ps4-openorbis.cmake, which is the toolchain file; defaulted here too
  # so that a configure naming a different toolchain fails on a path rather than on an empty string.
  if(NOT ORBIS_COMPAT_DIR)
    set(ORBIS_COMPAT_DIR "$ENV{HOME}/src-ps4/orbis-compat")
  endif()
  if(NOT EXISTS "${ORBIS_COMPAT_DIR}/vkloader/CMakeLists.txt")
    message(FATAL_ERROR "PS4 build needs orbis-compat at ${ORBIS_COMPAT_DIR} - set -DORBIS_COMPAT_DIR=<path>")
  endif()
  if(NOT EXISTS "${OG_PS4_SRC}/og_ps4_boot.h")
    message(FATAL_ERROR "PS4 build needs this title's ps4/ sources at ${OG_PS4_SRC}")
  endif()

  # ⚠ RADV IS THE ONLY BACKEND ON THIS BRANCH, and there is nothing here to select it with. The title
  # runs on orbis-mesa through orbis-compat's vkloader, with ACO compiling the SPIR-V at run time and
  # NO SHADER BAKE - which is why the GNM bake apparatus is absent rather than switched off.
  #
  # Two forces stood here until 2026-08-20 and neither did anything: TEMPEST_BUILD_GNM has no reader
  # in the Tempest fork, and TEMPEST_PS4_VULKAN survives only in a commit message that describes it in
  # the past tense. Engine/CMakeLists.txt forces TEMPEST_BUILD_VULKAN ON for PS4, which is the real one.
  add_subdirectory("${ORBIS_COMPAT_DIR}/vkloader" ps4-vkloader)

  # libc++ 11 ships a synopsis-only <concepts>; ZenKit and OpenGothic are C++20.
  # ps4/cxx20shim/ says what this is and what it is not.
  set(OG_PS4_SHIM "${OG_PS4_SRC}/cxx20shim")
  # musl + libc++ 11 also promote ~190 -Wconversion/-Wabsolute-value warnings out of the
  # math headers alone, in files nobody here owns; the -Werror further down is dropped
  # for the same reason. Both come back the moment the SDK's libc++ is new enough.
  set(OG_PS4_CXXFLAGS "-I${OG_PS4_SHIM}" "-include" "${OG_PS4_SHIM}/ps4_cxx20_shim.h"
                      -Wno-error "-Wno-#warnings")
  # Twice, and it is not redundant: a target's COMPILE_OPTIONS is seeded from the
  # directory property WHEN THE TARGET IS CREATED, and add_executable() above already
  # ran. add_compile_options reaches ZenKit/dmusic/bullet (added later, and the reason
  # the shim exists at all); target_compile_options reaches this executable.
  add_compile_options(${OG_PS4_CXXFLAGS})
  target_compile_options(${PROJECT_NAME} PRIVATE ${OG_PS4_CXXFLAGS})

  # ZenKit's mmap stays ON, and is forced rather than left to ZenKit's own default: it
  # was briefly forced OFF here, and a CMake cache remembers that forever - a build
  # directory configured during that window would silently keep the 2.51 GiB read path
  # even after the line was deleted. Forcing the value states the decision and makes an
  # old build directory converge on it.
  set(ZK_ENABLE_MMAP ON CACHE INTERNAL "")
  #
  # It was briefly forced off on the theory that the
  # console's `VFS disk signature not recognized: ""` came from a mapping that was not
  # backed by the file. THE CONSOLE REFUTED THAT: og_ps4_boot.cpp's probe reported
  # `mmap agrees with read()` byte for byte on hardware. The real fault was the SIZE
  # handed to mmap - the SDK's `struct stat` disagrees with the kernel's layout, so
  # fstat reported 128 for a 64857-byte archive and ZenKit mapped 128 bytes, putting the
  # signature at offset 256 outside the mapping. The overlay's orbis_stat.cpp fixes that at the
  # source. Turning mmap off would have cost 2.51 GiB of resident VDF to work
  # around a bug that was never in mmap.

  # ps4-app: the klog/netlog tee and the termination policy. It is opt-in - optional/ is deliberately
  # not part of liborbis-compat.a - and this is where a title opts in.
  add_subdirectory("${ORBIS_COMPAT_DIR}/optional" ps4-common)
  target_link_libraries(${PROJECT_NAME} ps4-app)
  target_link_libraries(${PROJECT_NAME} ps4-vkloader)
  target_include_directories(${PROJECT_NAME} PRIVATE "${OG_PS4_SRC}")
  # ⚠ stat/mmap/mem/paths MOVED to orbis-compat. They correct this SDK rather than this title, and
  # Mesa and the CTS need the same corrections; they were tied here only by calling Tempest's
  # ps4_log, which is now registered into the overlay's own hook by ps4_app_init. The linker rule
  # that kept them sources - an archive member nobody references is never pulled in - is answered
  # there by --whole-archive, which the toolchain file passes for liborbis-compat.a.
  target_sources(${PROJECT_NAME} PRIVATE
    "${OG_PS4_SRC}/og_ps4_boot.cpp"
    # The system on-screen keyboard, for naming a savegame. An ordinary TU, with no strong-symbol
    # trick to it - the three files that needed one (stat, mmap, paths) are the overlay's now, per
    # the note above. It is here rather than in Engine/ because it is OpenGothic's save menu that
    # needs it. See og_ps4_ime.h for what the ime probes measured.
    "${OG_PS4_SRC}/og_ps4_ime.cpp"
    # TEMPEST_BUILD_AUDIO is forced OFF for PS4 and libTempest.a therefore defines no
    # sound symbols, while the headers stay unguarded - so one of these two links an
    # implementation of them. Both files are always compiled and each guards its whole
    # body on OG_SOUND_NULL, so exactly one defines the symbols and there is no CMake
    # branch to keep in step with the knob.
    #
    #   default          og_sound_orbis.cpp - the real sceAudioOut mixer
    #   OG_SOUND_NULL=1  og_sound_null.cpp  - silent, and the control rung
    #
    # The null one is not a fallback to reach for when something sounds wrong: it is the
    # build that proves a defect is in the audio path rather than beside it.
    "${OG_PS4_SRC}/og_sound_null.cpp"
    "${OG_PS4_SRC}/og_sound_orbis.cpp")
  # ⚠ AND HERE IS THE KNOB, which the paragraph above described for months without providing.
  # Nothing set OG_SOUND_NULL: no option(), no set(), no flag in ps4/build.sh. The control rung was
  # documented, compiled and unreachable without hand-editing a command line - the exact inverse of
  # OG_FOG_DIAG, which was a knob with no code and came out on the same review.
  option(OG_SOUND_NULL "PS4: link the silent control-rung sound backend instead of the mixer" OFF)
  if(OG_SOUND_NULL)
    target_compile_definitions(${PROJECT_NAME} PRIVATE OG_SOUND_NULL=1)
    message(STATUS "OpenGothic: OG_SOUND_NULL=1 - the SILENT backend; this build makes no sound by design")
  endif()
  # libSceAudioOut for the port, libSceUserService because sceAudioOutOpen takes a user id
  # and OpenOrbis' own sample initialises the service before opening. Whether the port
  # strictly needs the service is not stated anywhere; the audio probe reported both return codes
  # and both were 0x0 on hardware, so this keeps the sequence that is known to work.
  # -lSceImeDialog and -lSceCommonDialog for the savegame-name keyboard, -lSceSysmodule because
  # the IME dialog is a sysmodule loaded at runtime rather than an eboot dependency.
  target_link_libraries(${PROJECT_NAME} -lSceAudioOut -lSceUserService
                        -lSceImeDialog -lSceCommonDialog -lSceSysmodule)
endif()

if(MSVC)
  add_definitions(-D_USE_MATH_DEFINES)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
  add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
  add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4800")
endif()

if(NOT MSVC)
  if(PS4)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wconversion -Wno-strict-aliasing)
  else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wconversion -Wno-strict-aliasing -Werror)
  endif()
  if(CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-class-memaccess)
  endif()
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "7.1" AND NOT APPLE AND NOT ${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-format-truncation)
  endif()
  if(UNIX)
    # -rdynamic is needed to allow obtaining meaningful backtraces
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU") # gcc
    # required by pthread_setname_np()
    add_definitions(-D_GNU_SOURCE)
  endif()
endif()

if(WIN32)
  add_definitions(-DNOMINMAX)
  if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup" )
  elseif(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mwindows -lpthread")
  else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mwindows -lstdc++fs")
  endif()
elseif(IOS)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
  set_directory_properties("${CMAKE_SOURCE_DIR}" PROPERTIES
    PROPERTY XCODE_STARTUP_PROJECT "${PROJECT_NAME}")
  set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "opengothic.gothic2"
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer")
elseif(APPLE)
  enable_language(OBJCXX)
endif()

if(IOS)
  file(GLOB_RECURSE ObjCSOURCES "game/*.mm")
elseif(APPLE)
  target_link_libraries(${PROJECT_NAME} "-framework AppKit")
  file(GLOB_RECURSE ObjCSOURCES "game/*.mm")
endif()

## OpenGothic

# sources
file(GLOB_RECURSE OPENGOTHIC_SOURCES
    "game/*.h"
    "game/*.cpp")

target_sources(${PROJECT_NAME} PRIVATE ${OPENGOTHIC_SOURCES} ${ObjCSOURCES} icon.rc)

# shaders
add_subdirectory(shader)
target_link_libraries(${PROJECT_NAME} GothicShaders)

include_directories("game")

## submodule dependencies

# edd-dbg
if(WIN32)
  add_subdirectory(lib/edd-dbg)
  include_directories(lib/edd-dbg/include)
  target_link_libraries(${PROJECT_NAME} edd_dbg)
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME} shlwapi DbgHelp)
elseif(UNIX)
  target_link_libraries(${PROJECT_NAME} -lpthread -ldl)
endif()

# ZenKit
set(BUILD_SQUISH_WITH_SSE2     OFF CACHE INTERNAL "")
set(ZK_ENABLE_ASAN             OFF CACHE INTERNAL "")
set(ZK_ENABLE_DEPRECATION      OFF CACHE INTERNAL "")

add_subdirectory(lib/ZenKit)
target_link_libraries(${PROJECT_NAME} zenkit)

# dmusic
set(DM_BUILD_STATIC            ON  CACHE INTERNAL "")
set(DM_ENABLE_ASAN             OFF CACHE INTERNAL "")

add_subdirectory(lib/dmusic)
target_link_libraries(${PROJECT_NAME} dmusic)

# TinySoundFont
add_definitions(-DTSF_NO_STDIO)
include_directories(lib/TinySoundFont)

# Tempest
if(WIN32)
  set(TEMPEST_BUILD_SHARED ON  CACHE INTERNAL "")
else()
  set(TEMPEST_BUILD_SHARED OFF CACHE INTERNAL "")
endif()
add_subdirectory(lib/Tempest/Engine)
include_directories(lib/Tempest/Engine/include)
target_link_libraries(${PROJECT_NAME} Tempest)

# zip
include_directories(lib/miniz)
add_definitions(-DMINIZ_NO_STDIO)
add_library(miniz STATIC "lib/miniz/miniz.h" "lib/miniz/miniz.c")
target_link_libraries(${PROJECT_NAME} miniz)

# bullet physics
set(BULLET2_MULTITHREADING       ON  CACHE INTERNAL "")
set(USE_MSVC_RUNTIME_LIBRARY_DLL ON  CACHE INTERNAL "")
set(USE_GRAPHICAL_BENCHMARK      OFF CACHE INTERNAL "")
set(BUILD_BULLET2_DEMOS          OFF CACHE INTERNAL "") # No samples
set(BUILD_OPENGL3_DEMOS          OFF CACHE INTERNAL "") # No samples
set(BUILD_UNIT_TESTS             OFF CACHE INTERNAL "") # No tests
set(BUILD_CPU_DEMOS              OFF CACHE INTERNAL "") # No samples
set(BUILD_EXTRAS                 OFF CACHE INTERNAL "") # No bugs
set(GLFW_BUILD_EXAMPLES          OFF CACHE INTERNAL "")
set(GLFW_BUILD_TESTS             OFF CACHE INTERNAL "")
set(GLFW_BUILD_DOCS              OFF CACHE INTERNAL "")
set(BUILD_BULLET3                OFF CACHE INTERNAL "") # Can use bullet2, bullet3 wants to build examples...
add_subdirectory(lib/bullet3)
if(NOT MSVC)
  target_compile_options(BulletDynamics         PRIVATE -Wno-argument-outside-range)
  target_compile_options(BulletInverseDynamics  PRIVATE -Wno-argument-outside-range)
  target_compile_options(BulletSoftBody         PRIVATE -Wno-argument-outside-range)
  target_compile_options(Bullet3Common          PRIVATE -Wno-argument-outside-range)
  target_compile_options(BulletCollision        PRIVATE -Wno-argument-outside-range)
  target_compile_options(LinearMath             PRIVATE -Wno-argument-outside-range)
endif()
if(NOT MSVC AND NOT APPLE AND NOT ${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
  target_compile_options(BulletDynamics         PRIVATE -Wno-stringop-overflow)
  target_compile_options(BulletInverseDynamics  PRIVATE -Wno-stringop-overflow)
  target_compile_options(BulletSoftBody         PRIVATE -Wno-stringop-overflow)
  target_compile_options(Bullet3Common          PRIVATE -Wno-stringop-overflow)
  target_compile_options(BulletCollision        PRIVATE -Wno-stringop-overflow)
  target_compile_options(LinearMath             PRIVATE -Wno-stringop-overflow)
endif()

include_directories(lib/bullet3/src)
target_link_libraries(${PROJECT_NAME} BulletDynamics BulletCollision LinearMath)

# script for launching in binary directory
if(WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/scripts/Gothic2Notr.bat
        ${CMAKE_CURRENT_BINARY_DIR}/opengothic/Gothic2Notr.bat)
endif()

# in debug mode, enable sanitizers
if(${CMAKE_BUILD_TYPE} MATCHES "Debug")
  add_compile_options(-fsanitize=address)
  add_compile_options(-fsanitize=leak)
  add_link_options   (-fsanitize=address)
  add_link_options   (-fsanitize=leak)
endif()

# PS4 packaging. `.elf` because an emulator loads the plain ELF and every demo in this fork named
# its executable that way; the eboot and the .pkg are the console path.
#
# TMPS10021 continues the fork's own TITLE_ID run (TMPS10001..TMPS10020 are the demos
# and the gapi suite). THE PACKAGE CONTAINS NO GAME
# DATA and never will: Gothic II is not redistributable, and the title discovers an
# installation on the console's storage at run time (ps4/og_ps4_boot.h).
if(PS4)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")
  ps4_create_eboot(${PROJECT_NAME})
  ps4_create_pkg(${PROJECT_NAME}
    TITLE_ID      TMPS10021
    TITLE         "OpenGothic"
    # 16 chars, which is the cap: make-pkg.sh does `${CONTENT_LABEL:0:16}` with no warning. This
    # read TEMPESTOPENGOTHIC - 17 - so every package ever built was already
    # IV0000-TMPS10021_00-TEMPESTOPENGOTHI and this file named something that never shipped. The
    # value below is the one already on the console, so the content id does not move.
    CONTENT_LABEL TEMPESTOPENGOTHI)
endif()

# installation
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
