#!/usr/bin/env python3
"""
migrate.py - Stop-and-copy live migration test script.
Requires: scripts/build.sh (to build migration samples)
Requires: iokerneld already running on each node (see README)

Usage:
  Node 0 (sender):    scripts/migrate.py sender <port> [--src-ip IP --dst-ip IP]
  Node 1 (receiver):  scripts/migrate.py receiver [--src-ip IP --dst-ip IP]
  Initiator:          scripts/migrate.py initiator <port> [--src-ip IP --dst-ip IP]

For kv_service (larger heap workload):
  Node 0 (sender):    scripts/migrate.py sender 8080 --service kv --num-keys 10000 --value-size 1024
  Initiator:          scripts/migrate.py initiator 8080 --service kv
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, "..")
BUILD_DIR = os.path.join(ROOT_DIR, "build", "junction")
JUNCTION_RUN = os.path.join(BUILD_DIR, "junction_run")
JUNCTION_CTL = os.path.join(ROOT_DIR, "build", "junction-ctl", "junction-ctl")
MIGRATION_BUILD_DIR = os.path.join(BUILD_DIR, "samples", "migration")
COUNTER_SVC = os.path.join(MIGRATION_BUILD_DIR, "counter_service")
KV_SVC = os.path.join(MIGRATION_BUILD_DIR, "kv_service")

DEFAULT_SRC_IP = "10.10.1.1"
DEFAULT_DST_IP = "10.10.1.2"

CALADAN_CONFIG_COMMON = """\
host_netmask 255.255.255.0
runtime_kthreads 4
runtime_spinning_kthreads 0
runtime_guaranteed_kthreads 4
runtime_priority lc
runtime_quantum_us 0"""

SRC_IP = DEFAULT_SRC_IP
DST_IP = DEFAULT_DST_IP


def generate_caladan_config(host_addr, host_gateway):
    """Write a Caladan config to a temp file and return its path."""
    content = f"host_addr {host_addr}\nhost_gateway {host_gateway}\n{CALADAN_CONFIG_COMMON}\n"
    f = tempfile.NamedTemporaryFile(mode="w", suffix=".config",
                                    prefix="caladan_", delete=False)
    f.write(content)
    f.close()
    return f.name


def send_cmd(ip, port, cmd, timeout=5):
    """Send a command to a service and return the response."""
    with socket.create_connection((ip, port), timeout=timeout) as s:
        s.sendall((cmd + "\n").encode())
        return s.recv(4096).decode().strip()


def wait_for_service(ip, port, timeout=30):
    """Poll until the service responds, return time when it first responds."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            send_cmd(ip, port, "GET", timeout=0.1)
            return time.perf_counter()
        except OSError:
            time.sleep(0.001)
    raise TimeoutError(f"Service at {ip}:{port} did not come up within {timeout}s")


def kill_leftover():
    subprocess.run(["sudo", "pkill", "-f", "junction_run"],
                   capture_output=True)
    time.sleep(1)


def cmd_sender(port, skip_file_pages, verbose, service, num_keys, value_size):
    kill_leftover()
    config = generate_caladan_config(SRC_IP, DST_IP)
    if service == "kv":
        svc_bin = KV_SVC
        svc_args = [str(port), str(num_keys), str(value_size)]
        print(f"==> Starting kv_service on {SRC_IP}:{port} "
              f"({num_keys} keys, {value_size} B each)")
    else:
        svc_bin = COUNTER_SVC
        svc_args = [str(port)]
        print(f"==> Starting counter_service on {SRC_IP}:{port}")
    loglevel = "6" if verbose else "5"
    cmd = [
        "sudo", "-E", JUNCTION_RUN, config, "--snapshot_enabled",
        "--loglevel", loglevel,
    ]
    if skip_file_pages:
        cmd.append("--skip_file_pages")
    cmd += ["--", svc_bin] + svc_args
    try:
        subprocess.run(cmd)
    finally:
        os.unlink(config)


def cmd_receiver(verbose):
    kill_leftover()
    config = generate_caladan_config(DST_IP, SRC_IP)
    print("==> Migration server listening on port 44")
    loglevel = "6" if verbose else "5"
    cmd = ["sudo", "-E", JUNCTION_RUN, config, "--snapshot_enabled",
           "--loglevel", loglevel]
    try:
        subprocess.run(cmd)
    finally:
        os.unlink(config)


def do_migrate(port, scatter_copy):
    """Trigger migration and return total migration time in us."""
    pid = subprocess.check_output(
        [JUNCTION_CTL, SRC_IP, "ps"]
    ).decode().strip().strip("[]").split(",")[0].strip()
    print(f"==> Migrating pid={pid} from {SRC_IP} to {DST_IP}:44")

    t_start = time.perf_counter()
    migrate_cmd = [JUNCTION_CTL, SRC_IP, "migrate"]
    if scatter_copy:
        migrate_cmd.append("--scatter-copy")
    migrate_cmd += [pid, DST_IP, "44"]
    subprocess.run(migrate_cmd, check=True)

    t_dst_up = wait_for_service(DST_IP, port)
    total_us = (t_dst_up - t_start) * 1e6

    # Verify source is down
    print("==> Verifying source is no longer serving (expect error):")
    try:
        send_cmd(SRC_IP, port, "GET", timeout=2)
        print("WARNING: source still responding!")
    except OSError:
        print("==> Source confirmed down.")

    return total_us


def cmd_initiator_counter(port, scatter_copy):
    print(f"==> Incrementing counter on source ({SRC_IP}):")
    for _ in range(3):
        print(send_cmd(SRC_IP, port, "INC"))
    print("==> Counter state before migration:")
    print(send_cmd(SRC_IP, port, "GET"))

    total_us = do_migrate(port, scatter_copy)

    print("==> Counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))
    print("==> Incrementing counter on destination:")
    for _ in range(3):
        print(send_cmd(DST_IP, port, "INC"))
    print("==> Final counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))

    print(f"\n==> Total migration time: {total_us:.1f} us")


def cmd_initiator_kv(port, scatter_copy):
    print(f"==> KV store stats on source ({SRC_IP}):")
    print(send_cmd(SRC_IP, port, "STATS"))
    pre_cksum = send_cmd(SRC_IP, port, "CHECKSUM")
    print(f"==> Checksum before migration: {pre_cksum}")

    total_us = do_migrate(port, scatter_copy)

    print("==> KV store stats on destination:")
    print(send_cmd(DST_IP, port, "STATS"))
    post_cksum = send_cmd(DST_IP, port, "CHECKSUM")
    print(f"==> Checksum after migration:  {post_cksum}")

    if pre_cksum == post_cksum:
        print("==> CHECKSUM MATCH — state preserved correctly")
    else:
        print("==> CHECKSUM MISMATCH — data corruption detected!")

    print("==> Writing new key on destination:")
    print(send_cmd(DST_IP, port, "SET test_key hello_from_dst"))
    print(send_cmd(DST_IP, port, "GET test_key"))

    print(f"\n==> Total migration time: {total_us:.1f} us")


def add_ip_args(parser):
    parser.add_argument("--src-ip", default=DEFAULT_SRC_IP,
                        help=f"source node IP (default: {DEFAULT_SRC_IP})")
    parser.add_argument("--dst-ip", default=DEFAULT_DST_IP,
                        help=f"destination node IP (default: {DEFAULT_DST_IP})")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-v", action="store_true", help="verbose logging")
    sub = parser.add_subparsers(dest="role", required=True)

    s = sub.add_parser("sender")
    s.add_argument("port", type=int)
    add_ip_args(s)
    s.add_argument("--skip-file-pages", action="store_true",
                   help="skip file-backed read-only pages during migration")
    s.add_argument("--service", choices=["counter", "kv"], default="counter",
                   help="service to run (default: counter)")
    s.add_argument("--num-keys", type=int, default=10000,
                   help="number of keys to pre-load (kv only, default: 10000)")
    s.add_argument("--value-size", type=int, default=1024,
                   help="value size in bytes (kv only, default: 1024)")

    r = sub.add_parser("receiver")
    add_ip_args(r)

    i = sub.add_parser("initiator")
    i.add_argument("port", type=int)
    add_ip_args(i)
    i.add_argument("--scatter-copy", action="store_true",
                   help="use scatter-copy migration instead of ELF format")
    i.add_argument("--service", choices=["counter", "kv"], default="counter",
                   help="service being migrated (default: counter)")

    args = parser.parse_args()

    global SRC_IP, DST_IP
    SRC_IP = args.src_ip
    DST_IP = args.dst_ip

    if args.role == "sender":
        cmd_sender(args.port, args.skip_file_pages, args.v,
                   args.service, args.num_keys, args.value_size)
    elif args.role == "receiver":
        cmd_receiver(args.v)
    elif args.role == "initiator":
        if args.service == "kv":
            cmd_initiator_kv(args.port, args.scatter_copy)
        else:
            cmd_initiator_counter(args.port, args.scatter_copy)


if __name__ == "__main__":
    main()
