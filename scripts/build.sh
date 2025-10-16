#!/bin/bash

function usage() {
    echo "usage: scripts/test.sh [-s|--snap-samples] [-p|--permissive-seccomp] [-d|--debug]" >&2
    exit 255
}

SNAP_SAMPLES="OFF"
PERMISSIVE_SECCOMP="OFF"
DEBUG="OFF"
WRITEABLE_LINUX_FS="OFF"
CI="OFF"

for arg in "$@"; do
    shift
    case "${arg}" in
        '--help'|'-h') usage ;;
        '--snap-samples'|'-s') SNAP_SAMPLES="ON" ;;
        '--permissive-seccomp'|'-p') PERMISSIVE_SECCOMP="ON";;
        '--writeable-linux-fs'|'-w') WRITEABLE_LINUX_FS="ON";;
        '--debug'|'-d') DEBUG="ON";;
        '--ci'|'-c') CI="ON";; 
    esac
done

set -xe

# Globals
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/../
if [ "${DEBUG}" = "ON" ];
then
    BUILD_TYPE=Debug
    BUILD_DIR=${ROOT_DIR}/build-debug
else
    BUILD_TYPE=Release
    BUILD_DIR=${ROOT_DIR}/build
fi
BIN_DIR=${ROOT_DIR}/bin
CMAKE=${BIN_DIR}/bin/cmake

if [ "${CI}" = "ON" ];
then
    . "${SCRIPT_DIR}"/submodule_check.sh --ci

    echo "CI Mode: Patching Junction's Caladan API calls..."
    TARGET_FILE="${ROOT_DIR}/junction/bindings/net.h"
    sed -i -E '/udp_readv_from2/,/peek, nonblocking\);/ s/peek, nonblocking\);/peek, nonblocking, nullptr\);/' "$TARGET_FILE"
    sed -i -E '/udp_writev_to2/,/raddr, nonblocking\);/ s/raddr, nonblocking\);/raddr, nonblocking, nullptr\);/' "$TARGET_FILE"
    cat "$TARGET_FILE" 
else
    . "${SCRIPT_DIR}"/submodule_check.sh
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

$CMAKE -DWRITEABLE_LINUX_FS="${WRITEABLE_LINUX_FS}" -DPERMISSIVE_SECCOMP="${PERMISSIVE_SECCOMP}" -DSNAPSHOT_SAMPLES="${SNAP_SAMPLES}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ..
make -j "$(nproc)"
