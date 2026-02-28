#!/bin/bash
# BLE Provisioner Wrapper Script
# Runs provisioner.py for exactly 5 minutes, then gracefully shuts down

set -e

SCRIPT_DIR="/opt/rooted-ble"
VENV_PYTHON="${SCRIPT_DIR}/.venv/bin/python3"
PROVISIONER="${SCRIPT_DIR}/provisioner.py"
LOG_FILE="/var/log/rooted-ble.log"

# Duration in seconds (5 minutes)
DURATION=300

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "${LOG_FILE}"
}

cleanup() {
    log "Stopping BLE provisioner..."

    # Kill the Python process if running
    if [ -n "${PROVISIONER_PID}" ] && kill -0 "${PROVISIONER_PID}" 2>/dev/null; then
        kill -TERM "${PROVISIONER_PID}" 2>/dev/null
        wait "${PROVISIONER_PID}" 2>/dev/null || true
    fi

    # Disable Bluetooth adapter for power saving
    log "Disabling Bluetooth adapter for power saving..."
    hciconfig hci0 down 2>/dev/null || true

    log "BLE provisioner stopped. Bluetooth window closed."
    exit 0
}

# Trap signals for graceful shutdown
trap cleanup SIGTERM SIGINT SIGQUIT EXIT

log "=========================================="
log "Starting BLE provisioner (${DURATION}s window)..."

# Ensure Bluetooth adapter is up
log "Powering on Bluetooth adapter..."
rfkill unblock bluetooth 2>/dev/null || true
sleep 1
if ! hciconfig hci0 up 2>/dev/null; then
    log "Warning: Could not bring up Bluetooth adapter with hciconfig, trying bluetoothctl..."
    bluetoothctl power on 2>/dev/null || true
fi
sleep 1

# Start the provisioner in background
"${VENV_PYTHON}" "${PROVISIONER}" >> "${LOG_FILE}" 2>&1 &
PROVISIONER_PID=$!

log "Provisioner started with PID ${PROVISIONER_PID}"

# Wait for the duration or until process exits
ELAPSED=0
while [ ${ELAPSED} -lt ${DURATION} ]; do
    # Check if provisioner is still running
    if ! kill -0 "${PROVISIONER_PID}" 2>/dev/null; then
        log "Provisioner exited early (likely completed onboarding or error)"
        break
    fi

    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

# Time's up - cleanup will be called via EXIT trap
