#!/usr/bin/env bash
# Helper script to launch/stop the full 7-process TCU simulation cluster on Host PC:
#   1x Virtual Companion MCU (tcu-mcu-sim) + 6x Linux Telematics Daemons (--mock)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
CONFIG_FILE="${CONFIG_FILE:-${REPO_ROOT}/config/tcu_config.yaml}"
RUNTIME_DIR="${RUNTIME_DIR:-/tmp/tcu}"
PID_DIR="${RUNTIME_DIR}/pids"
LOG_DIR="${RUNTIME_DIR}/logs"

SERVICES=(
    "tcu-mcu-sim"
    "tcu-mcu-ipcd"
    "tcu-platformd"
    "tcu-ecalld"
    "tcu-svtd"
    "tcu-otad"
    "tcu-diagd"
)

start_cluster() {
    mkdir -p "${PID_DIR}" "${LOG_DIR}"

    echo "[TCU] Starting Virtual Companion MCU (tcu-mcu-sim)..."
    "${BUILD_DIR}/tcu-mcu-sim" --socket "${RUNTIME_DIR}/mcu_spi.sock" \
        >"${LOG_DIR}/tcu-mcu-sim.log" 2>&1 &
    echo $! >"${PID_DIR}/tcu-mcu-sim.pid"
    sleep 0.2

    echo "[TCU] Starting Intra-SoC UDS Broker & MCU Bridge (tcu-mcu-ipcd)..."
    "${BUILD_DIR}/tcu-mcu-ipcd" --config "${CONFIG_FILE}" --runtime-dir "${RUNTIME_DIR}" --mock \
        >"${LOG_DIR}/tcu-mcu-ipcd.log" 2>&1 &
    echo $! >"${PID_DIR}/tcu-mcu-ipcd.pid"
    sleep 0.3

    for daemon in tcu-platformd tcu-ecalld tcu-svtd tcu-otad tcu-diagd; do
        echo "[TCU] Starting ${daemon}..."
        "${BUILD_DIR}/${daemon}" --config "${CONFIG_FILE}" --runtime-dir "${RUNTIME_DIR}" --mock \
            >"${LOG_DIR}/${daemon}.log" 2>&1 &
        echo $! >"${PID_DIR}/${daemon}.pid"
        sleep 0.15
    done

    echo "[TCU] All 7 processes started (runtime_dir=${RUNTIME_DIR}, logs=${LOG_DIR})."
}

stop_cluster() {
    if [[ ! -d "${PID_DIR}" ]]; then
        echo "[TCU] No PID directory at ${PID_DIR}."
        return 0
    fi
    for (( idx=${#SERVICES[@]}-1 ; idx>=0 ; idx-- )); do
        svc="${SERVICES[idx]}"
        pid_file="${PID_DIR}/${svc}.pid"
        if [[ -f "${pid_file}" ]]; then
            pid="$(cat "${pid_file}")"
            if kill -0 "${pid}" 2>/dev/null; then
                kill "${pid}" 2>/dev/null || true
                wait "${pid}" 2>/dev/null || true
            fi
            rm -f "${pid_file}"
        fi
    done
    echo "[TCU] Cluster stopped cleanly."
}

case "${1:-}" in
    start)
        start_cluster
        ;;
    stop)
        stop_cluster
        ;;
    restart)
        stop_cluster
        start_cluster
        ;;
    status)
        "${BUILD_DIR}/tcu-ctl" --config "${CONFIG_FILE}" --runtime-dir "${RUNTIME_DIR}" status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac

