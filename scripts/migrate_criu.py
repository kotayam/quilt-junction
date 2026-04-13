#!/usr/bin/env python3
"""
migrate_criu.py - Disk-less stop-and-copy migration benchmark using CRIU.
Follows https://criu.org/Disk-less_migration

Memory pages are streamed directly to the destination page-server over TCP.
Only small metadata images are copied via scp.

Requires: criu built and installed (see README)
Requires: passwordless ssh/scp from node-0 to node-1

Usage:
  Node 1 (receiver): scripts/migrate_criu.py receiver
  Node 0 (sender):   scripts/migrate_criu.py sender <service_port>
                     scripts/migrate_criu.py migrate <service_port>
"""

import argparse
import os
import socket
import subprocess
import time

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, "..")
COUNTER_SVC = os.path.join(ROOT_DIR, "build", "junction", "samples", "migration", "counter_service")

DST_IP = "10.10.1.2"       # Junction/Caladan subnet IP (service reachability)
DST_SSH = "node-1"         # SSH alias for the destination host (see ~/.ssh/config)
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


def cmd_receiver():
    subprocess.run(["sudo", "mkdir", "-p", DUMP_DIR], check=True)
    subprocess.run(["sudo", "mount", "-t", "tmpfs", "none", DUMP_DIR], check=True)
    print(f"==> Starting CRIU page-server on port {PAGE_SERVER_PORT} ...")
    subprocess.run([
        "sudo", "criu", "page-server",
        "--images-dir", DUMP_DIR,
        "--port", str(PAGE_SERVER_PORT),
    ], check=True)
    print("==> Page-server done. Waiting for metadata images from sender ...")


def cmd_sender(port):
    print(f"==> Starting counter_service on port {port}")
    proc = subprocess.Popen([COUNTER_SVC, str(port)])
    print(f"==> PID: {proc.pid}. Run 'migrate {port}' on this node to trigger migration.")
    proc.wait()


def cmd_migrate(port):
    print(f"==> Incrementing counter on source (localhost):")
    for _ in range(3):
        print(send_cmd("127.0.0.1", port, "INC"))
    print("==> Counter state before migration:")
    print(send_cmd("127.0.0.1", port, "GET"))

    pid = int(subprocess.check_output(["pgrep", "-f", "counter_service"])
              .decode().strip().splitlines()[0])
    print(f"==> Dumping pid={pid}, streaming pages to {DST_IP}:{PAGE_SERVER_PORT}")

    subprocess.run(["sudo", "mkdir", "-p", DUMP_DIR], check=True)
    subprocess.run(["sudo", "mount", "-t", "tmpfs", "none", DUMP_DIR],
                   capture_output=True)  # ignore if already mounted

    t_start = time.monotonic()

    # Dump: stream pages to dst page-server, leave process stopped
    subprocess.run([
        "sudo", "criu", "dump",
        "--tree", str(pid),
        "--images-dir", DUMP_DIR,
        "--leave-stopped",
        "--page-server", "--address", DST_IP, "--port", str(PAGE_SERVER_PORT),
    ], check=True)
    t_src_down = time.monotonic()

    # Measure metadata size (pages are already on dst)
    dump_size = sum(
        os.path.getsize(os.path.join(DUMP_DIR, f))
        for f in os.listdir(DUMP_DIR)
    )

    # Copy small metadata images to dst
    print("==> Copying metadata images to destination ...")
    subprocess.run([
        "scp", "-r", f"{DUMP_DIR}/.", f"{DST_SSH}:{DUMP_DIR}/"
    ], check=True)

    # Restore on dst
    print("==> Restoring on destination ...")
    subprocess.run([
        "ssh", DST_SSH,
        f"sudo criu restore --images-dir {DUMP_DIR} --shell-job -d"
    ], check=True)

    t_dst_up = wait_for_service(DST_IP, port)

    # Kill stopped process on source
    subprocess.run(["sudo", "kill", "-9", str(pid)], capture_output=True)

    downtime_ms = (t_dst_up - t_src_down) * 1000
    total_ms = (t_dst_up - t_start) * 1000

    print("==> Verifying source is no longer serving:")
    try:
        send_cmd("127.0.0.1", port, "GET", timeout=2)
        print("WARNING: source still responding!")
    except OSError:
        print("==> Source confirmed down.")

    print("==> Counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))
    print("==> Incrementing counter on destination:")
    for _ in range(3):
        print(send_cmd(DST_IP, port, "INC"))
    print("==> Final counter state on destination:")
    print(send_cmd(DST_IP, port, "GET"))

    print(f"\n==> Metadata size:        {dump_size // 1024} KiB")
    print(f"==> Downtime:             {downtime_ms:.1f} ms")
    print(f"==> Total migration time: {total_ms:.1f} ms")

    # Cleanup
    subprocess.run(["sudo", "umount", DUMP_DIR], capture_output=True)
    subprocess.run(["ssh", DST_SSH, f"sudo umount {DUMP_DIR}"], capture_output=True)


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
