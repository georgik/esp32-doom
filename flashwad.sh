#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRBOOM_WAD="${SCRIPT_DIR}/prboom.wad"
DOOM_WAD="${SCRIPT_DIR}/DOOM1.WAD"
PRBOOM_OFFSET="0x210000"
DOOM_OFFSET="0x290000"
BAUD_RATE="${BAUD_RATE:-460800}"
CHIP="${CHIP:-esp32-s3}"

find_idf_path() {
    if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
        printf '%s\n' "${IDF_PATH}"
        return 0
    fi

    local candidates=(
        "${HOME}/esp/v6.0/esp-idf"
        "${HOME}/esp/esp-idf"
        "${HOME}/esp-idf"
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/export.sh" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

find_serial_port() {
    if [[ -n "${1:-}" ]]; then
        printf '%s\n' "$1"
        return 0
    fi

    if [[ -n "${ESPPORT:-}" ]]; then
        printf '%s\n' "${ESPPORT}"
        return 0
    fi

    if [[ -n "${ESPTOOL_PORT:-}" ]]; then
        printf '%s\n' "${ESPTOOL_PORT}"
        return 0
    fi

    local candidates=(
        /dev/cu.usbmodem*
        /dev/cu.wchusbserial*
        /dev/cu.SLAB_USBtoUART*
        /dev/ttyACM*
        /dev/ttyUSB*
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -e "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

if [[ ! -f "${PRBOOM_WAD}" ]]; then
    echo "Missing ${PRBOOM_WAD}" >&2
    exit 1
fi

if [[ ! -f "${DOOM_WAD}" ]]; then
    echo "Missing ${DOOM_WAD}" >&2
    exit 1
fi

IDF_PATH="$(find_idf_path)" || {
    echo "Could not locate ESP-IDF. Set IDF_PATH to your ESP-IDF directory and rerun." >&2
    exit 1
}

PORT="$(find_serial_port "${1:-}")" || {
    echo "Could not detect a serial port. Pass it as the first argument or set ESPPORT." >&2
    exit 1
}

# Load the ESP-IDF tools into the current shell so esptool.py resolves from the active environment.
. "${IDF_PATH}/export.sh" >/dev/null

esptool.py \
    --chip "${CHIP}" \
    --port "${PORT}" \
    --baud "${BAUD_RATE}" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio \
    --flash_freq 40m \
    --flash_size detect \
    "${PRBOOM_OFFSET}" "${PRBOOM_WAD}" \
    "${DOOM_OFFSET}" "${DOOM_WAD}"
