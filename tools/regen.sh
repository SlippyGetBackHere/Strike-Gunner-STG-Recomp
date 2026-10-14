#!/usr/bin/env bash
# Regenerate src/gen/*.c for Strike Gunner S.T.G from a verified ROM.
#
# Generated C is derived from copyrighted ROM data and is never committed;
# every developer regenerates from their own copy. The ROM digests come from
# rom_identity.txt, which the build and the release workflow read too, so
# there is one place to change when a revision changes.
#
# Flags:
#   --rom <path>  ROM to generate from. Defaults to SNESRECOMP_ROM, a known
#                 filename at the repo root, then the path in rom.cfg (which
#                 the setup wizard writes). The ROM does not have to live in
#                 the repo — keeping it on your own drive is the better
#                 habit, and SNESRECOMP_ROM sets it once for a shell.
#   --no-verify   skip the ROM digest check (for a revision this project has
#                 not been pinned to yet — expect the generated C to differ)
#   --cfg-roots   seed analysis from every func declaration in recomp/*.cfg
#   -h|--help     this message
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

VERIFY=1
CFG_ROOTS=0
ROM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rom) ROM=$2; shift 2 ;;
    --no-verify) VERIFY=0; shift ;;
    --cfg-roots) CFG_ROOTS=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -n '/^# /p' | sed 's/^# //'; exit 0 ;;
    *) echo "regen.sh: unknown flag: $1 (try --help)" >&2; exit 2 ;;
  esac
done

SNESRECOMP_ROOT="${SNESRECOMP_ROOT:-snesrecomp}"
CLI="$SNESRECOMP_ROOT/snesrecomp_cli.py"
if [ ! -f "$CLI" ]; then
  echo "regen.sh: $CLI missing — run: git submodule update --init --recursive" >&2
  exit 1
fi

PYTHON="${PYTHON:-$(command -v python3 || command -v python || true)}"
if [ -z "$PYTHON" ]; then
  echo "regen.sh: no python3 on PATH" >&2
  exit 1
fi

# The framework parses rom_identity.txt (the same parser CI uses), so this
# script carries no copy of a digest to forget on a revision bump.
IDENTITY="$SNESRECOMP_ROOT/tools/rom_identity.py"
EXPECTED_CRC32="${SNESRECOMP_EXPECTED_CRC32:-$("$PYTHON" "$IDENTITY" "$ROOT/rom_identity.txt" --get expected_crc32)}"
EXPECTED_SHA256="${SNESRECOMP_EXPECTED_SHA256:-$("$PYTHON" "$IDENTITY" "$ROOT/rom_identity.txt" --get expected_sha256)}"

# The framework owns the search: --rom, then SNESRECOMP_ROM, then a known
# name at the root, then rom.cfg (the path the setup wizard recorded).
RESOLVE=(resolve-rom --project-root "$ROOT"
         --name "Strike Gunner S.T.G (USA).sfc" --name "strikegunnerstg.sfc" --name "strikegunnerstg.smc")
if [ -n "$ROM" ]; then RESOLVE+=(--rom "$ROM"); fi
if ! ROM=$("$PYTHON" "$CLI" "${RESOLVE[@]}"); then
  echo "regen.sh: pass --rom /path/to/Strike Gunner S.T.G (USA).sfc, set SNESRECOMP_ROM, put it" >&2
  echo "          at the repo root, or record its path in rom.cfg. You must" >&2
  echo "          legally own a copy of Strike Gunner S.T.G." >&2
  exit 1
fi
ROM=${ROM%$'\r'}  # Windows Python ends lines with CRLF

VERIFY_ARGS=()
if [ "$VERIFY" -eq 1 ]; then
  VERIFY_ARGS=(--expected-crc32 "$EXPECTED_CRC32" --expected-sha256 "$EXPECTED_SHA256")
  echo "== Verifying $ROM =="
  "$PYTHON" "$CLI" verify-rom --rom "$ROM" "${VERIFY_ARGS[@]}"
else
  echo "== Skipping ROM verification (--no-verify) =="
fi

EMIT_ARGS=(--rom "$ROM" --cfg-dir recomp --out-dir src/gen
           --analysis-backend native)
if [ "$CFG_ROOTS" -eq 1 ]; then EMIT_ARGS+=(--cfg-roots); fi
# Coverage captures seed the analyzer with the entry points the interpreter
# was actually seen reaching (snesrecomp/docs/COVERAGE_FEEDBACK.md). Without
# them only the vectors are roots and almost nothing is compiled. They hold
# addresses and hit counts, never ROM data. Add a capture to recomp/coverage/
# to widen what is compiled; never remove one without re-checking the build.
#
# Each capture comes from a different build, and only the emitter's
# historical input validates captures one at a time instead of refusing to
# merge them; `snesrecomp_cli.py generate` does not forward it. So the
# emitter and the funcs.h sync are run directly here, after the CLI has
# verified the ROM above.
for profile in recomp/coverage/*.json recomp/coverage/*.jsonl; do
  [ -f "$profile" ] && EMIT_ARGS+=(--historical-profile-manifest "$profile")
done
ANALYZER="$SNESRECOMP_ROOT/recompiler-rs/target/release/snesrecomp-analyze"
for candidate in "$ANALYZER.exe" "$ANALYZER"; do
  if [ -f "$candidate" ]; then export SNESRECOMP_NATIVE_ANALYZER="$candidate"; break; fi
done

echo "== Generating src/gen =="
mkdir -p src/gen
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_emit.py" "${EMIT_ARGS[@]}"
"$PYTHON" "$SNESRECOMP_ROOT/tools/v2_sync_funcs_h.py" --cfg-dir recomp --out recomp/funcs.h

echo
echo "Done. Build with:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
