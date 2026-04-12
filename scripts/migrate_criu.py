#!/usr/bin/env python3
"""
migrate_criu.py - Stop-and-copy migration benchmark using CRIU page-server.
Streams the dump directly over TCP — comparable to Junction's migration.

Requires: sudo apt install -y criu
Requires: counter_service binary built (scripts/build.sh)

Usage:
  Node 0 (sender):   scripts/migrate_criu.py sender <service_port>
                     scripts/migrate_criu.py migrate <service_port>
  Node 1 (receiver): scripts/migrate_criu.py receiver
"""

import argparse
import os
import socket
import subprocess
import time

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, "..")
COUNTER_SVC = os.path.join(ROOT_DIR, "build", "junction", "samples", "migration", "counter_service")

SRC_IP = "127.0.0.1"  # sender and migrate run on the same node
DST_IP = "10.10.1.2"  # host IP of receiver node
DUMP_DIR = "/tmp/criu_dump"
PAGE_SERVER_PORT = 9999


def send_cmd(ip, port, cmd, timeout=5):
    with socket.create_connection((ip, port), timeout=timeout) as s:
        s.sendall((cmd + "\n").encode())
        return s.recv(64).decode().strip()


def wait_for_service(ip, port, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            send_cmd(ip, port, "GET", timeout=0.1)
            return time.monotonic()
        except OSError:
            time.sleep(0.01)
    raise TimeoutError(f"Service at {ip}:{port} did not come up within {timeout}s")


def cmd_sender(port):
    print(f"==> Starting counter_service on port {port}")
    proc = subprocess.Popen([COUNTER_SVC, str(port)])
    print(f"==> PID: {proc.pid}. Run 'migrate {port}' on this node to trigger migration.")
    proc.wait()


def cmd_receiver():
    os.makedirs(DUMP_DIR, exist_ok=True)
    print(f"==> Starting CRIU page-server on port {PAGE_SERVER_PORT} ...")
    subprocess.run([
        "sudo", "criu", "page-server",
        "--port", str(PAGE_SERVER_PORT),
        "-D", DUMP_DIR,
    ], check=True)
    print("==> Page-server done, restoring ...")
    subprocess.run([
        "sudo", "criu", "restore", "-D", DUMP_DIR, "--shell-job", "-d",
    ], check=True)
    print("==> Restore complete.")


def cmd_migrate(port):
    print(f"==> Incrementing counter on source ({SRC_IP}):")
    for _ in range(3):
        print(send_cmd(SRC_IP, port, "INC"))
    print("==> Counter state before migration:")
    print(send_cmd(SRC_IP, port, "GET"))

    pid = int(subprocess.check_output(["pgrep", "-f", "counter_service"])
              .decode().strip().splitlines()[0])
    print(f"==> Dumping pid={pid}, streaming to {DST_IP}:{PAGE_SERVER_PORT}")

    os.makedirs(DUMP_DIR, exist_ok=True)

    t_start = time.monotonic()
    subprocess.run([
        "sudo", "criu", "dump",
        "-t", str(pid),
        "-D", DUMP_DIR,
        "--shell-job",
        "--page-server", "--address", DST_IP, "--port", str(PAGE_SERVER_PORT),
    ], check=True)
    t_src_down = time.monotonic()

    dump_size = sum(
        os.path.getsize(os.path.join(DUMP_DIR, f))
        for f in os.listdir(DUMP_DIR)
    )

    print("==> Verifying source is no longer serving:")
    try:
        send_cmd(SRC_IP, port, "GET", timeout=2)
        print("WARNING: source still responding!")
    except OSError:
        print("==> Source confirmed down.")

    t_dst_up = wait_for_service(DST_IP, port)

    downtime_ms = (t_dst_up - t_src_down) * 1000
    total_ms = (t_dst_up - t_start) * 1000

    print("==> Counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))
    print("==> Incrementing counter on destination:")
    for _ in range(3):
        print(send_cmd(DST_IP, port, "INC"))
    print("==> Final counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))

    print(f"\n==> Dump size:            {dump_size // 1024} KiB")
    print(f"==> Downtime:             {downtime_ms:.1f} ms")
    print(f"==> Total migration time: {total_ms:.1f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="role", required=True)
    sub.add_parser("sender").add_argument("port", type=int)
    sub.add_parser("receiver")
    sub.add_parser("migrate").add_argument("port", type=int)
    args = parser.parse_args()

    if args.role == "sender":
        cmd_sender(args.port)
    elif args.role == "receiver":
        cmd_receiver()
    elif args.role == "migrate":
        cmd_migrate(args.port)


if __name__ == "__main__":
    main()
