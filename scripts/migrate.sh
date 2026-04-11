#!/bin/bash
# migrate.sh - Stop-and-copy live migration test script.
#
# Usage:
#   Node 1 (sender):   scripts/migrate.sh sender   <service_port>
#   Node 2 (receiver): scripts/migrate.sh receiver
#   Node 3 (initiator):scripts/migrate.sh initiator <src_ip> <dest_ip> <service_port>

set -e

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=${SCRIPT_DIR}/..
BUILD_DIR=${ROOT_DIR}/build/junction
JUNCTION_RUN=${BUILD_DIR}/junction_run
JUNCTION_CTL=${ROOT_DIR}/build/junction-ctl/junction-ctl
CONFIG=${BUILD_DIR}/caladan_test.config

start_iokerneld() {
  local caladan_dir=${ROOT_DIR}/lib/caladan
  sudo pkill iokerneld 2>/dev/null || true
  sleep 1
  sudo ${caladan_dir}/scripts/setup_machine.sh
  sudo ${caladan_dir}/iokerneld ias no_hw_qdel -- --allow 00:00.0 --vdev=net_tap0 \
    > /tmp/iokernel.log 2>&1 &
  while ! grep -q 'running dataplan' /tmp/iokernel.log; do
    sleep 0.3
    pgrep iokerneld > /dev/null || { echo "iokerneld failed"; exit 1; }
  done
  echo "iokerneld started"
}

case "$1" in
  sender)
    service_port=$2
    start_iokerneld
    sudo -E ${JUNCTION_RUN} ${CONFIG} --snapshot_enabled \
      -- ${BUILD_DIR}/counter_service ${service_port} &
    sleep 2
    for i in 1 2 3; do echo "INC" | nc -q1 localhost ${service_port}; done
    echo "==> Counter state before migration:"
    echo "GET" | nc -q1 localhost ${service_port}
    echo "==> Sender ready. Run the initiator to trigger migration."
    wait
    ;;

  receiver)
    start_iokerneld
    echo "==> Migration server listening on port 43"
    sudo -E ${JUNCTION_RUN} ${CONFIG} --snapshot_enabled
    ;;

  initiator)
    src_ip=$2
    dest_ip=$3
    service_port=$4
    pid=$(${JUNCTION_CTL} ${src_ip} ps | tr -d '[], ' | head -1)
    echo "==> Migrating pid=${pid} from ${src_ip} to ${dest_ip}:43"
    ${JUNCTION_CTL} ${src_ip} migrate ${pid} ${dest_ip} 43
    echo "==> Migration complete. Counter state on destination:"
    sleep 1
    echo "GET" | nc -q1 ${dest_ip} ${service_port}
    ;;

  *)
    echo "usage: $0 {sender <service_port> | receiver | initiator <src_ip> <dest_ip> <service_port>}"
    exit 1
    ;;
esac
