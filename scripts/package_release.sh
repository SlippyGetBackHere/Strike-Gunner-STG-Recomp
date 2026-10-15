#!/usr/bin/env bash
# Package a Strike Gunner S.T.G release: a SETUP PACK.
#
# The zip does not contain the game. It contains a setup program (the host
# built with -DSNESRECOMP_SETUP_HOST=ON, i.e. without recompiled code), the
# recompiler and this source tree. On first run the player picks their own
# ROM and the launcher recompiles and rebuilds locally. No ROM, no src/gen,
# no recomp/funcs.h ever leaves this tree in a zip -- the framework's packager
# enforces that mechanically.
#
# Usage:
#   scripts/package_release.sh [<build-dir>] [<platform-tag>] [--embed-toolchain]
#
# Both positionals are optional; BUILD_DIR and PLATFORM still work as env vars.
# Defaults: build, and the host's own <os>-<arch>. --embed-toolchain copies
# the cmake-clang-v1 pack named by RETCOMM_TOOLCHAIN_DIR into the zip so the
# player downloads nothing.
#
# Writes: dist/strikegunnerstg-<VERSION>-<platform-tag>.zip
set -euo pipefail

# A silent exit is the worst outcome a packaging step can have: the first
# Windows CI run died here with exit 1 and not one line of output. Name the
# command and line instead of leaving the reader to bisect a shell script.
trap 'echo "::error::$(basename "${BASH_SOURCE[0]}") failed at line ${LINENO}: ${BASH_COMMAND}" >&2' ERR


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

EXTRA=()
POS=()
for a in "$@"; do
  case "$a" in
    --embed-toolchain|--no-embed-toolchain) EXTRA+=("$a") ;;
    *) POS+=("$a") ;;
  esac
done
BUILD_DIR="${POS[0]:-${BUILD_DIR:-build}}"
PLATFORM="${POS[1]:-${PLATFORM:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)}}"

IMPL="snesrecomp/tools/ci/stage_setup_host.sh"
if [ ! -f "$IMPL" ]; then
  echo "package_release.sh: $IMPL missing — the snesrecomp pin predates setup packs." >&2
  echo "  Update the submodule: git submodule update --init --recursive" >&2
  exit 1
fi

# Bring the setup host up to date before staging it. The stager copies
# whatever executable the build directory holds, so a build that failed
# (or was never rerun after a pull) shipped the previous setup program
# with no warning. A clean build is a no-op here.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "package_release.sh: $BUILD_DIR is not configured. Configure the setup host first:" >&2
  echo "  cmake -S . -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_SETUP_HOST=ON" >&2
  exit 1
fi
if ! cmake --build "$BUILD_DIR"; then
  echo "package_release.sh: building the setup host in $BUILD_DIR failed; nothing was packaged." >&2
  echo "  A setup host must be built from a tree with no generated code: src/gen/*.c" >&2
  echo "  and recomp/funcs.h must not exist (they appear if the launcher's Generate" >&2
  echo "  & rebuild is run inside this checkout)." >&2
  exit 1
fi

# The ROM hint and digests come from rom_identity.txt -- the same file the
# build compiles in and tools/regen.sh verifies against -- so a revision bump
# is one edit. stage_setup_host.sh already needs Python, so this does too.
# Windows installs it as python, not python3.
PY="$(command -v python3 || command -v python || true)"
if [ -z "$PY" ]; then
  echo "package_release.sh: python3 (or python) not found on PATH" >&2
  exit 1
fi
IDENTITY_TOOL="snesrecomp/tools/rom_identity.py"
ROM_FILE="$("$PY" "$IDENTITY_TOOL" rom_identity.txt --get rom_file)"
ROM_SHA256="$("$PY" "$IDENTITY_TOOL" rom_identity.txt --get expected_sha256)"
ROM_CRC32="$("$PY" "$IDENTITY_TOOL" rom_identity.txt --get expected_crc32)"
ROM_FILE=${ROM_FILE%$'\r'}; ROM_SHA256=${ROM_SHA256%$'\r'}; ROM_CRC32=${ROM_CRC32%$'\r'}

# mods/ is the catalog the build stages beside the executable, and the package
# a netplay host transfers to a guest. Add --runtime-dir <name> for any other
# directory the game loads exe-relative (translations, ...).
exec bash "$IMPL" \
  --runtime-dir mods \
  --build-dir "$BUILD_DIR" \
  --artifact "$PLATFORM" \
  --exe-name StrikeGunnerSTGSNESRecomp \
  --zip-prefix strikegunnerstg \
  --display-name "Strike Gunner S.T.G" \
  --rom-hint "$ROM_FILE" \
  --rom-sha256 "$ROM_SHA256" \
  --rom-crc32 "$ROM_CRC32" \
  "${EXTRA[@]+"${EXTRA[@]}"}"
