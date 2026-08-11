#!/bin/sh
# Build + chay toan bo test tren desktop. Khong can CMake, khong can RTOS.
#
#   sh scripts/run_tests.sh
#
# Doi trinh bien dich neu can:
#   CC=/c/Qt/Tools/mingw1310_64/bin/gcc.exe sh scripts/run_tests.sh

set -e
CC=${CC:-gcc}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build_host}
mkdir -p "$OUT"

CORE=$(ls "$ROOT"/core/src/*.c)
SERVICES=$(ls "$ROOT"/services/*/*.c)
TESTS=$(ls "$ROOT"/tests/unit/*.c "$ROOT"/tests/behavior/*.c "$ROOT"/tests/support/*.c)

# shellcheck disable=SC2086
"$CC" -std=c11 -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -DIPC_PORT_HOST \
    -I"$ROOT/core/include" \
    -I"$ROOT/services" \
    -I"$ROOT/app" \
    -I"$ROOT/tests/support" \
    $CORE $SERVICES "$ROOT/app/app.c" "$ROOT/port/port_host.c" $TESTS \
    -o "$OUT/ipc_tests" -lpthread

"$OUT/ipc_tests"
