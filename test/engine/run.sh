#!/usr/bin/env bash
#
# test/engine/run.sh: build and run the standalone engine tests.
#
# Usage: test/engine/run.sh [seed [cases]]
#
# Builds the engine with the oracle test twice: once plain with -Werror,
# once with AddressSanitizer and UndefinedBehaviorSanitizer.  Needs a C
# compiler (CC, default cc) that supports -fsanitize.  Build output goes to
# a temporary directory that is removed afterwards.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "$here/../.." && pwd)/src"
out="$(mktemp -d)"
trap 'rm -f "$out"/test_engine "$out"/test_engine_san "$out"/test_engine_collide; rmdir "$out"' EXIT

CC="${CC:-cc}"
WARN=(-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)
SRCS=("$src"/engine/*.c "$here/oracle.c" "$here/test_engine.c")

$CC "${WARN[@]}" -O2 -I"$src" "${SRCS[@]}" -lm -o "$out/test_engine"
$CC "${WARN[@]}" -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$src" "${SRCS[@]}" -lm -o "$out/test_engine_san"

# A third build forces every cache key to the same hash, so the cache must
# tell components apart by comparing keys, not hashes.
$CC "${WARN[@]}" -O2 -DGX_TEST_HASH_COLLIDE -I"$src" "${SRCS[@]}" -lm -o "$out/test_engine_collide"

"$out/test_engine" "$@"
"$out/test_engine_collide" "$@"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$out/test_engine_san" "$@"
