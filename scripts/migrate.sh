#!/bin/bash
# migrate.sh - Stop-and-copy live migration test script.
# Requires: scripts/build.sh -s (to build snapshot samples)
# Requires: iokerneld already running (see README)
#
# Usage:
#   Node 1 (sender):    scripts/migrate.sh sender   <service_port>
#   Node 2 (receiver):  scripts/migrate.sh receiver
#   Node 3 (initiator): scripts/migrate.sh initiator <src_ip> <dest_ip> <service_port>

set -e

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
BUILD_DIR=${ROOT_DIR}/build/junction
JUNCTION_RUN=${BUILD_DIR}/junction_run
JUNCTION_CTL=${ROOT_DIR}/build/junction-ctl/junction-ctl
CONFIG=${BUILD_DIR}/caladan_test.config
COUNTER_SVC=${BUILD_DIR}/samples/snapshots/c/counter_service

case "$1" in
  sender)
    service_port=$2
    sudo -E ${JUNCTION_RUN} ${CONFIG} --snapshot_enabled \
      -- ${COUNTER_SVC} ${service_port} &
    sleep 2
    for i in 1 2 3; do echo "INC" | nc -q1 localhost ${service_port}; done
    echo "==> Counter state before migration:"
    echo "GET" | nc -q1 localhost ${service_port}
    echo "==> Sender ready. Run the initiator to trigger migration."
    wait
    ;;

  receiver)
    echo "==> Migration server listening on port 44"
    sudo -E ${JUNCTION_RUN} ${CONFIG} --snapshot_enabled
    ;;

  initiator)
    src_ip=$2
    dest_ip=$3
    service_port=$4
    pid=$(${JUNCTION_CTL} ${src_ip} ps | tr -d '[], ' | head -1)
    echo "==> Migrating pid=${pid} from ${src_ip} to ${dest_ip}:44"
    ${JUNCTION_CTL} ${src_ip} migrate ${pid} ${dest_ip} 44
    echo "==> Migration complete. Counter state on destination:"
    sleep 1
    echo "GET" | nc -q1 ${dest_ip} ${service_port}
    ;;

  *)
    echo "usage: $0 {sender <service_port> | receiver | initiator <src_ip> <dest_ip> <service_port>}"
    exit 1
    ;;
esac
