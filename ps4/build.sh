#!/usr/bin/env bash
# Cross-build OpenGothic for the PlayStation 4.
#
#   ps4/build.sh [--work <dir>] [--jobs N] [--orbis-compat <dir>] [--sound-null]
#
# Builds THIS checkout in place. lib/Tempest and lib/ZenKit are expected to be the forks
# carrying the PS4 work; everything the build produces lives under --work, nothing under
# the repository.
#
# ⚠ THERE IS NO PATCH QUEUE ANY MORE, AND NO SCRATCH CLONE. Both existed because the PS4
# changes to OpenGothic had nowhere to live: the previous script cloned an untouched
# checkout and applied a patch queue to it, "until the maintainer decides
# how the OpenGothic side of this port is tracked". That is decided - there are forks - so
# the changes are commits here and the build reads them directly.
#
# ⚠ THE GNM SIDE IS GONE WITH IT. --tune, --diag and AMDLLPC selected shader-bake and
# backend knobs for the GNM route, which this branch does not carry: RADV compiles the
# SPIR-V at run time through ACO, so there is nothing to bake and no compiler to pin.
#
# WHAT IS NOT PACKAGED: game data. Not by this script, not by the .pkg it produces. The
# title finds an installation at run time - ps4/og_ps4_boot.h.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${HOME}/.cache/opengothic-ps4"
JOBS="$(nproc)"
SOUND_NULL=OFF
# The platform overlay: the toolchain file, the SDK corrections, the Vulkan C ABI and the log
# channel.
#
# ⚠ The six lines that cannot be shared - see orbis-compat/scripts/ps4/orbis-env.sh. Sibling
# directory before the old personal default, because that is what a fresh clone of the orbis-ports
# organisation looks like: the repositories next to each other.
for _c in "${ORBIS_COMPAT_DIR:-}" "$(dirname "${BASH_SOURCE[0]}")/../../orbis-compat" "${HOME}/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || {
  echo "!! orbis-compat not found - clone https://github.com/orbis-ports/orbis-compat next to this" >&2
  echo "   repository, or set ORBIS_COMPAT_DIR / pass --orbis-compat <dir>" >&2
  exit 1
}
. "${ORBIS_COMPAT_DIR}/scripts/ps4/orbis-env.sh"
ORBIS_COMPAT="${ORBIS_COMPAT_DIR}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --work) WORK="$2"; shift 2 ;;
    --orbis-compat) ORBIS_COMPAT="$2"; ORBIS_COMPAT_DIR="$2"; export ORBIS_COMPAT_DIR; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    # The silent control rung. Not a fallback for when something sounds wrong: it is the build that
    # separates "the defect is in the audio path" from "the defect is beside it".
    --sound-null) SOUND_NULL=ON; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BUILD="${WORK}/build"

# ⚠ THE THREE TREES THAT CARRY THE PORT ARE CHECKED, BECAUSE A STALE ONE BUILDS CLEANLY.
#
# ⚠ AND THIS BLOCK WAS ITSELF STALE FOR A DAY. It checked lib/Tempest/ps4/vkloader/vkloader.h,
# which moved to the overlay on 2026-08-20, so every run of this script exited 1 before compiling
# anything - and the toolchain path below named a Tempest cmake/ directory that is now empty. A
# guard that names a moved file is worse than no guard: it fails builds that are correct. When a
# file moves, its guard moves with it.
#
# orbis-compat must carry the loader and the toolchain file - the C ABI in front of RADV, because
# there is no Vulkan loader on this console and there cannot be one. lib/Tempest must be the fork
# with the Orbis SystemApi backend, without which there is no window, no pad and no clock.
# lib/ZenKit must be the one with VfsMountMode, without which resources.cpp does not compile; and
# were it to compile, the mount would fall back to mmap, which populates eagerly here and hangs the
# machine on ~2.7 GiB of archives.
#
# Checked by a file each rather than by a revision: any of them may be a symlink to a working tree,
# a checked-out branch or a fork's commit, and the build cares only that the code is there.
[[ -f "${ORBIS_COMPAT}/cmake/ps4-openorbis.cmake" && -f "${ORBIS_COMPAT}/vkloader/vkloader.h" ]] || {
  echo "!! no orbis-compat at ${ORBIS_COMPAT} - clone it, or pass --orbis-compat <dir>" >&2
  exit 1
}
[[ -f "${ROOT}/lib/Tempest/Engine/system/api/ps4api.h" ]] || {
  echo "!! lib/Tempest has no Orbis SystemApi backend - point it at the Tempest fork" >&2
  exit 1
}
grep -q "VfsMountMode" "${ROOT}/lib/ZenKit/include/zenkit/Vfs.hh" 2>/dev/null || {
  echo "!! lib/ZenKit has no VfsMountMode - point it at the ZenKit fork" >&2
  exit 1
}

# ⚠ CMAKE_POLICY_VERSION_MINIMUM IS FOR A DEPENDENCY, NOT FOR THIS PROJECT.
#
# ZenKit's vendor/CMakeLists.txt fetches doctest unconditionally - not gated on
# ZK_BUILD_TESTS - and doctest 2.4.9 declares a cmake_minimum_required that CMake 4 refuses
# outright, so the configure stops before anything of ours is read. ZK_BUILD_TESTS=OFF is
# passed anyway so the suite is not built for a console that cannot run it.
#
# The flag lowers the policy floor for the whole tree, which is blunt; it is here rather
# than in ZenKit because a cross-build should not be the thing that changes how a library
# fetches its test framework.
mkdir -p "${BUILD}"

# ⚠ THE DRIVER IS PASSED, NOT LEFT TO A DEFAULT. vkloader/CMakeLists.txt defaults
# ORBIS_MESA_BUILD to ~/.cache/orbis-mesa/mesa/build-orbis - the patch-queue era's path, which on
# 2026-08-23 still held a driver from the previous day. Nothing announced it; the title would simply
# have linked the wrong RADV and every measurement taken from the run would have been about changes
# it did not contain. orbis-env.sh resolves the checkout; this prints what it found.
orbis_announce_driver

echo "== configuring ${BUILD}"
cmake -S "${ROOT}" -B "${BUILD}" \
      -DCMAKE_TOOLCHAIN_FILE="${ORBIS_COMPAT}/cmake/ps4-openorbis.cmake" \
      -DORBIS_COMPAT_DIR="${ORBIS_COMPAT}" \
      -DORBIS_MESA_BUILD="${ORBIS_MESA_BUILD}" \
      -DORBIS_MESA_SRC="${ORBIS_MESA_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DPS4_BUILD_PKG=ON \
      -DZK_BUILD_TESTS=OFF \
      -DOG_SOUND_NULL="${SOUND_NULL}" \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "== building"
cmake --build "${BUILD}" --target Gothic2Notr -j "${JOBS}"

echo
echo "ELF:   ${BUILD}/opengothic/Gothic2Notr.elf"
echo "eboot: ${BUILD}/opengothic/eboot.bin"
echo "pkg:   ${BUILD}/opengothic/TMPS10021.pkg (when PkgTool.Core is present)"
