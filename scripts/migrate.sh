#!/bin/bash
# migrate.sh - Stop-and-copy live migration test script.
# Requires: scripts/build.sh -s (to build snapshot samples)
# Requires: iokerneld already running (see README)
#
# Usage:
#   Node 1 (sender):    scripts/migrate.sh sender   <service_port>
#   Node 2 (receiver):  scripts/migrate.sh receiver
#   Initiator:          scripts/migrate.sh initiator <service_port>

set -e

# Kill any leftover junction_run processes before starting
sudo pkill -f junction_run 2>/dev/null || true
sleep 1

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
BUILD_DIR=${ROOT_DIR}/build/junction
JUNCTION_RUN=${BUILD_DIR}/junction_run
JUNCTION_CTL=${ROOT_DIR}/build/junction-ctl/junction-ctl
MIGRATION_BUILD_DIR=${BUILD_DIR}/samples/migration
SERVICE_CONFIG=${MIGRATION_BUILD_DIR}/caladan_service.config
DST_CONFIG=${MIGRATION_BUILD_DIR}/caladan_migration_dst.config
COUNTER_SVC=${MIGRATION_BUILD_DIR}/counter_service

SRC_IP=10.10.1.1
DST_IP=10.10.1.2

case "$1" in
  sender)
    service_port=$2
    sudo -E ${JUNCTION_RUN} ${SERVICE_CONFIG} --snapshot_enabled \
      -- ${COUNTER_SVC} ${service_port} &
    echo "==> Sender ready. Run the initiator to trigger migration."
    wait
    ;;

  receiver)
    echo "==> Migration server listening on port 44"
    sudo -E ${JUNCTION_RUN} ${DST_CONFIG} --snapshot_enabled
    ;;

  initiator)
    service_port=$2
    echo "==> Incrementing counter on source (${SRC_IP}):"
    for i in 1 2 3; do echo "INC" | nc -q1 ${SRC_IP} ${service_port}; done
    echo "==> Counter state before migration:"
    echo "GET" | nc -q1 ${SRC_IP} ${service_port}
    pid=$(${JUNCTION_CTL} ${SRC_IP} ps | tr -d '[], ' | head -1)
    echo "==> Migrating pid=${pid} from ${SRC_IP} to ${DST_IP}:44"
    ${JUNCTION_CTL} ${SRC_IP} migrate ${pid} ${DST_IP} 44
    echo "==> Verifying source is no longer serving (expect timeout/error):"
    echo "GET" | nc -q1 -w2 ${SRC_IP} ${service_port} || echo "==> Source confirmed down."
    echo "==> Counter state on destination:"
    sleep 1
    echo "GET" | nc -q1 ${DST_IP} ${service_port}
    ;;

  *)
    echo "usage: $0 {sender <service_port> | receiver | initiator <service_port>}"
    exit 1
    ;;
esac
