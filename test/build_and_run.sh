#!/bin/sh
# Build + chay test tren desktop. Khong can CMake, khong can RTOS.
#
#   sh test/build_and_run.sh
#
# Doi CC neu may ban dung trinh bien dich khac:
#   CC=/c/Qt/Tools/mingw1310_64/bin/gcc.exe sh test/build_and_run.sh

set -e
CC=${CC:-gcc}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build_host}
mkdir -p "$OUT"

"$CC" -std=c11 -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -DIPC_PORT_HOST \
    -I"$ROOT/include" -I"$ROOT/test" -I"$ROOT/services" \
    "$ROOT/src/ipc_message.c" \
    "$ROOT/src/ipc_clock.c" \
    "$ROOT/src/ipc_looper.c" \
    "$ROOT/src/ipc_service.c" \
    "$ROOT/src/ipc_supervisor.c" \
    "$ROOT/src/ipc_timer.c" \
    "$ROOT/src/ipc_watchdog.c" \
    "$ROOT/src/ipc_health.c" \
    "$ROOT/src/ipc_config.c" \
    "$ROOT/src/ipc_event.c" \
    "$ROOT/src/ipc_event_group.c" \
    "$ROOT/services/app.c" \
    "$ROOT/services/drivers.c" \
    "$ROOT/services/svc_config.c" \
    "$ROOT/services/svc_sensor.c" \
    "$ROOT/services/svc_processor.c" \
    "$ROOT/services/svc_uploader.c" \
    "$ROOT/services/svc_health.c" \
    "$ROOT/src/port_host.c" \
    "$ROOT/test/test_main.c" \
    "$ROOT/test/test_timer.c" \
    "$ROOT/test/test_watchdog.c" \
    "$ROOT/test/test_config.c" \
    "$ROOT/test/test_health.c" \
    "$ROOT/test/test_bus.c" \
    "$ROOT/test/test_system.c" \
    -o "$OUT/ipc_tests" -lpthread

"$OUT/ipc_tests"
