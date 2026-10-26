#!/usr/bin/env python3
"""Run one bounded native bhyve network-debug case on a FreeBSD worker.

Every guest is created under a hard bound: `timeout -k GRACE LIMIT bhyve`
plus a watchdog that runs `bhyvectl --destroy` if bhyve is still present
after LIMIT + DESTROY_AFTER seconds.  bhyve treats SIGTERM as an ACPI power
button, which a guest stopped in DDB never answers, so SIGTERM alone does not
bound a panicked guest.  The case also destroys its own guest once its
observation is complete.  Nothing else is destroyed: the tap, image and
evidence remain in the new evidence directory.
"""
import argparse
import hashlib
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import tty
from pathlib import Path

CASES = ("d4", "d5", "d7-send-timeout", "d7-restart", "d7-malformed", "d7-link",
         "d1-loader-ops", "d2-hard-reset", "d3-handoff-vtnet", "d3-early-vtnet",
         "d3-early-em")
# Cases that boot the branch's EFI loader with its own debug console active.
LOADER_CASES = CASES[6:]
HOST_IP, GUEST_IP = "192.0.2.1", "192.0.2.2"
HDR = struct.Struct("!IIQII")
ACK = struct.Struct("!I")
MIN_MEMORY = 3 << 29  # 1.5 GiB of free plus inactive pages


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run_bounded_vm(vm, bhyve_args, limit, *, grace=10, destroy_after=20, log=None,
                   on_start=None):
    """Run bhyve under a hard deadline; return (status, watchdog_destroyed)."""
    command = ["sudo", "-n", "timeout", "-k", str(grace), str(limit), "bhyve",
               *bhyve_args, vm]
    proc = subprocess.Popen(command, stdout=log or subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    if on_start is not None:
        on_start(proc)
    destroyed = threading.Event()

    def watchdog():
        try:
            proc.wait(timeout=limit + destroy_after)
        except subprocess.TimeoutExpired:
            destroyed.set()
            destroy_vm(vm)

    thread = threading.Thread(target=watchdog, daemon=True)
    thread.start()
    status = proc.wait()
    thread.join()
    return status, destroyed.is_set()


def destroy_vm(vm):
    return subprocess.run(["sudo", "-n", "bhyvectl", "--destroy", f"--vm={vm}"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          check=False).returncode


def tree_sha256(directory):
    """Identity of a directory: SHA-256 over its sorted file digests."""
    root = Path(directory)
    lines = "".join(f"{sha256(p)}  {p.relative_to(root)}\n" for p in sorted(root.rglob("*"))
                    if p.is_file() and "__pycache__" not in p.parts)
    return hashlib.sha256(lines.encode()).hexdigest()


def admission(args):
    failures = []
    if Path(args.base).exists():
        failures.append("evidence directory exists")
    if subprocess.run(["pgrep", "-x", "bhyve"], stdout=subprocess.DEVNULL,
                      check=False).returncode == 0:
        failures.append("another bhyve guest is active")
    if Path(f"/dev/vmm/{args.vm}").exists():
        failures.append("VM name already exists")
    pages = [int(subprocess.run(["sysctl", "-n", name], stdout=subprocess.PIPE, text=True,
                                check=True).stdout) for name in
             ("vm.stats.vm.v_free_count", "vm.stats.vm.v_inactive_count", "hw.pagesize")]
    memory = (pages[0] + pages[1]) * pages[2]
    if memory < MIN_MEMORY:
        failures.append(f"free+inactive memory {memory} below {MIN_MEMORY}")
    identities = {"kernel": (args.kernel, args.kernel_sha256),
                  "loader": (args.loader, args.loader_sha256)}
    if args.case in LOADER_CASES:
        identities["loader_lua"] = (args.loader_lua, args.loader_lua_sha256)
        identities["tools"] = (args.tools, args.tools_sha256)
    else:
        identities["console"] = (Path(args.tools) / "debugnet-console.py", args.console_sha256)
        identities["proxy"] = (Path(args.tools) / "netgdb-proxy.py", args.proxy_sha256)
    if args.case == "d5":
        identities["init"] = (args.init, args.init_sha256)
    observed = {}
    for name, (path, expected) in identities.items():
        if path is None:
            failures.append(f"{name} not supplied")
            continue
        digest = tree_sha256(path) if Path(path).is_dir() else sha256(path)
        observed[name] = {"path": str(path), "sha256": digest}
        if expected and observed[name]["sha256"] != expected:
            failures.append(f"{name} identity mismatch")
        if not expected:
            failures.append(f"{name} identity not supplied")
    return failures, memory, observed


def prepare(args, base):
    root, esp = base / "root", base / "esp"
    (esp / "EFI/BOOT").mkdir(parents=True)
    (root / "boot/kernel").mkdir(parents=True)
    (root / "boot/defaults").mkdir(parents=True)
    subprocess.run(["cp", args.loader, str(esp / "EFI/BOOT/BOOTX64.EFI")], check=True)
    subprocess.run(["cp", args.kernel, str(root / "boot/kernel/kernel")], check=True)
    lua = args.loader_lua if args.case in LOADER_CASES else "/boot/lua"
    subprocess.run(["cp", "-R", lua, str(root / "boot/lua")], check=True)
    subprocess.run(["cp", "/boot/defaults/loader.conf", str(root / "boot/defaults/")],
                   check=True)
    if args.case == "d5":
        (root / "dev").mkdir()
        (root / "sbin").mkdir()
        subprocess.run(["cp", args.init, str(root / "sbin/init")], check=True)
    if args.case in LOADER_CASES:
        # The loader's own console starts (netdebug auto), exports the
        # identity and waits for the controller's commands.
        conf = ('autoboot_delay="NO"\nconsole="comconsole"\ncomconsole_speed="115200"\n'
                'vfs.root.mountfrom="ufs:/dev/nonexistent"\nnetdebug_enable="YES"\n'
                'netdebug.plain="1"\nnetdebug.interface="0"\n'
                f'netdebug.ip="{GUEST_IP}"\nnetdebug.peer="{HOST_IP}"\n')
        if args.case.startswith("d3-"):
            conf += 'netdebug.early="1"\n'
        if args.case.startswith("d3-early-"):
            conf += 'netdebug.early_panic="1"\n'
        (root / "boot/loader.conf").write_text(conf)
    else:
        (root / "boot/loader.conf").write_text(
            'autoboot_delay="0"\nconsole="comconsole"\ncomconsole_speed="115200"\n'
            'vfs.root.mountfrom="ufs:/dev/nonexistent"\nnetdebug.plain="1"\n'
            f'netdebug.hwaddr="{args.mac}"\nnetdebug.local_ip="{GUEST_IP}"\n'
            f'netdebug.peer_ip="{HOST_IP}"\nnetdebug.boot_id="{args.boot_id}"\n')
    for cmd, log in (
            (["makefs", "-t", "msdos", "-s", "64m", str(base / "esp.img"), str(esp)],
             "makefs-esp.log"),
            (["makefs", "-t", "ffs", "-B", "little", "-o", "version=2", "-s", "256m",
              str(base / "root.img"), str(root)], "makefs-root.log"),
            (["mkimg", "-s", "gpt", "-p", f"efi:={base / 'esp.img'}",
              "-p", f"freebsd-ufs:={base / 'root.img'}", "-o", str(base / "disk.img")],
             "mkimg.log")):
        with open(base / log, "wb") as handle:
            subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT, check=True)


class Serial:
    def __init__(self, path, log):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self.log = open(log, "wb", buffering=0)
        self.data = bytearray()
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        import select
        while not self.stop:
            if select.select([self.fd], [], [], .2)[0]:
                try:
                    chunk = os.read(self.fd, 4096)
                except OSError:
                    return
                with self.lock:
                    self.data.extend(chunk)
                self.log.write(chunk)

    def text(self):
        with self.lock:
            return bytes(self.data)

    def write(self, data):
        for byte in data:  # DDB polls one character at a time
            os.write(self.fd, bytes([byte]))
            time.sleep(.02)

    def close(self):
        self.stop = True
        self.thread.join(2)
        os.close(self.fd)
        self.log.close()


def wait_for(predicate, timeout, step=.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return predicate()


def events(path):
    try:
        return [json.loads(line) for line in Path(path).read_text().splitlines() if line]
    except FileNotFoundError:
        return []


def has_event(path, name, **match):
    return any(e["event"] == name and all(e.get(k) == v for k, v in match.items())
               for e in events(path))


def last_event(path, name):
    found = [e for e in events(path) if e["event"] == name]
    return found[-1] if found else None


def contains(path, needle):
    try:
        return needle in Path(path).read_bytes()
    except FileNotFoundError:
        return False


def console(args, base, name, *, resume=False):
    cmd = [sys.executable, str(Path(args.tools) / "debugnet-console.py"), "--bind", HOST_IP,
           "--boot-id", args.boot_id, "--timeout", str(args.controller_timeout),
           "--capture", str(base / "kernel-capture.bin"),
           "--events", str(base / f"{name}-events.jsonl"),
           "--state", str(base / "controller-state.json")]
    if resume:
        cmd.append("--resume")
    out = open(base / f"{name}.log", "wb")
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=out, stderr=subprocess.STDOUT)


def feed(proc, data):
    proc.stdin.write(data)
    proc.stdin.flush()


class Rsp:
    """Minimal GDB remote serial protocol client for raw stub checks."""

    def __init__(self, address, timeout=20):
        self.sock = socket.create_connection(address, timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.transcript = []

    def packet(self, body):
        checksum = sum(body.encode()) % 256
        wire = f"${body}#{checksum:02x}".encode()
        self.sock.sendall(wire)
        while True:
            match = re.search(rb"\$([^#]*)#([0-9a-fA-F]{2})", self.buf)
            if match:
                self.buf = self.buf[match.end():]
                self.sock.sendall(b"+")
                reply = match.group(1).decode("latin-1")
                self.transcript.append({"request": body, "reply": reply})
                return reply
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("GDB stub closed the connection")
            self.buf += chunk

    def close(self):
        self.sock.close()


def symbol_address(kernel_debug, symbol):
    output = subprocess.run(["nm", kernel_debug], stdout=subprocess.PIPE, text=True,
                            check=True).stdout
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == symbol:
            return int(parts[0], 16)
    raise LookupError(symbol)


def loader_console(args, base, name, *extra, stdin=False, timeout=60):
    cmd = [sys.executable, str(Path(args.tools) / "loader-console.py"), GUEST_IP,
           "--plain", "--bind", HOST_IP, "--timeout", str(timeout),
           "--output", str(base / f"{name}.out"), "--events", str(base / f"{name}.events"),
           "--last-stage", str(base / f"{name}.stage"), *extra]
    log = open(base / f"{name}.log", "wb")
    return subprocess.Popen(cmd, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                            stdout=log, stderr=subprocess.STDOUT)


def stage_record(base, name):
    try:
        return json.loads((base / f"{name}.stage").read_text())
    except (OSError, ValueError):
        return None


def run_loader_case(args, base, serial, results, observations, procs):
    """D1-D3 cases driven through the branch's loader console."""
    def check(name, value):
        results[name] = "pass" if value else "fail"

    def finished(proc, seconds):
        try:
            return proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            return None

    out = lambda name: (base / f"{name}.out").read_bytes() if (base / f"{name}.out").exists() else b""
    follow = ["--follow-kernel", "--kernel-transport", "debugnet", "--command", "boot"]
    if args.case == "d1-loader-ops":
        tftp = base / "tftp"
        tftp.mkdir()
        subprocess.run(["cp", args.kernel, str(tftp / "kernel")], check=True)
        subprocess.run(["cp", str(Path(args.tools) / "examples/staged-netdebug.lua"),
                        str(tftp / "staged-netdebug.lua")], check=True)
        yield
        ops = loader_console(
            args, base, "ops", "--tftp-root", str(tftp), "--tftp-address", HOST_IP,
            "--set", "daemonarchy.d1=set-ok", "--set", "daemonarchy.temp=x",
            "--unset", "daemonarchy.temp", "--show", "daemonarchy.d1",
            "--show", "daemonarchy.temp", "--kernel", "net0:/kernel",
            "--lua", "net0:/staged-netdebug.lua", "--command", "show staged.netdebug",
            "--command", "lsmod", "--stop", timeout=90)
        procs.append(ops)
        observations["ops_exit"] = finished(ops, 240)
        events = [json.loads(line) for line in (base / "ops.events").read_text().splitlines()]
        shown = {e["name"]: e["output"] for e in events if e["event"] == "variable_shown"}
        tftp_done = {e["file"]: e["bytes"] for e in events if e["event"] == "tftp_complete"}
        check("operations_completed", observations["ops_exit"] == 0)
        check("variable_set_and_shown", shown.get("daemonarchy.d1") == "set-ok")
        check("variable_unset", "not found" in shown.get("daemonarchy.temp", ""))
        check("kernel_downloaded", tftp_done.get("/kernel") == Path(args.kernel).stat().st_size)
        check("lua_downloaded_and_run", "/staged-netdebug.lua" in tftp_done and
              b"netdebug-stage: staged-lua" in out("ops") and b"ran" in out("ops"))
        check("console_stopped", any(e["event"] == "console_stopped" for e in events))
        record = stage_record(base, "ops")
        check("earliest_stage_loader", record is not None and
              record["earliest_stage"] == "efi-loader")
        # Stop: a new controller cannot pair until the console is restarted.
        refused = loader_console(args, base, "after-stop", "--show", "daemonarchy.d1",
                                 timeout=8)
        procs.append(refused)
        check("stop_refuses_pairing", finished(refused, 60) == 1)
        serial.write(b"netdebug auto\r")
        restarted = loader_console(args, base, "restart", "--show", "daemonarchy.d1",
                                   "--stop", timeout=30)
        procs.append(restarted)
        observations["restart_exit"] = finished(restarted, 90)
        events = [json.loads(line) for line in
                  (base / "restart.events").read_text().splitlines()]
        check("restart_pairs_with_state", observations["restart_exit"] == 0 and any(
            e["event"] == "variable_shown" and e["output"] == "set-ok" for e in events))
        return
    yield
    ctl = loader_console(args, base, "controller", *follow, stdin=True, timeout=60)
    procs.append(ctl)
    marker = lambda text: text.encode() in out("controller")
    if args.case == "d2-hard-reset":
        check("kernel_reached", wait_for(lambda: marker("mountroot>"), 180))
        reset = subprocess.run(["sudo", "-n", "bhyvectl", f"--vm={args.vm}", "--force-reset"],
                               capture_output=True, check=False)
        observations["force_reset_status"] = reset.returncode
        observations["controller_exit"] = finished(ctl, 120)
        record = stage_record(base, "controller")
        observations["last_stage"] = record
        check("controller_reports_silence", observations["controller_exit"] == 1)
        check("last_stage_retained", record is not None and record["stage"] in (
            "mountroot_prompt", "kernel-rootconf") and record["outcome"] == "idle_timeout")
        check("no_fabricated_panic", record is not None and record["panic_text"] is None)
        return
    if args.case == "d3-handoff-vtnet":
        check("early_transport_admitted", wait_for(lambda: marker("netdebug-stage: kernel-early "), 180))
        check("early_release", wait_for(lambda: marker("kernel-early-release"), 120))
        check("mountroot_console_reheralded", wait_for(
            lambda: marker("netdebug-stage: kernel-rootconf") and marker("mountroot>"), 120))
        feed(ctl, b"ufs:/dev/vtbd0p2\n")
        check("root_mount_attempt", wait_for(
            lambda: b"Trying to mount root from ufs:/dev/vtbd0p2" in serial.text(), 60))
        observations["controller_exit"] = finished(ctl, 60)
        check("controller_finished", observations["controller_exit"] == 0)
        text = out("controller")
        check("native_attach_after_release",
              text.find(b"kernel-early-release") < text.find(b"kernel-rootconf"))
        return
    # d3-early-*: deliberate panic while earlynet holds the NIC.
    check("early_transport_admitted", wait_for(lambda: marker("netdebug-stage: kernel-early "), 180))
    check("pre_driver_panic_received", wait_for(
        lambda: marker("deliberate pre-driver panic") and marker("db>"), 120))
    text = out("controller")
    driver = b"em0:" if args.case == "d3-early-em" else b"vtnet0:"
    check("native_driver_not_attached", driver not in text)
    feed(ctl, b"show pcpu\n")
    check("ddb_command_over_early_transport", wait_for(lambda: marker("cpuid"), 60))
    record = stage_record(base, "controller")
    observations["last_stage"] = record
    check("panic_text_recorded", record is not None and record["panic_text"] is not None and
          "deliberate pre-driver panic" in record["panic_text"])


def run_case(args, base, serial, results, observations, procs):
    if args.case in LOADER_CASES:
        yield from run_loader_case(args, base, serial, results, observations, procs)
        return
    capture = base / "kernel-capture.bin"
    ev = base / "controller-events.jsonl"
    mounted = lambda: b"Trying to mount root from ufs:/dev/vtbd0p2" in serial.text()
    listed = lambda: contains(capture, b"List of GEOM")

    def check(name, value):
        results[name] = "pass" if value else "fail"

    if args.case == "d5":
        proxy_log = open(base / "netgdb-proxy.log", "wb")
        proxy = subprocess.Popen([sys.executable, str(Path(args.tools) / "netgdb-proxy.py"),
                                  "--bind", HOST_IP, "--peer", GUEST_IP, "--timeout", "120",
                                  "--capture", str(base / "netgdb-capture.bin"),
                                  "--events", str(base / "netgdb-events.jsonl")],
                                 stdout=proxy_log, stderr=subprocess.STDOUT)
        procs.append(proxy)
    ctl = console(args, base, "controller")
    procs.append(ctl)
    yield
    if args.case in ("d4", "d5"):
        feed(ctl, b"?\nufs:/dev/vtbd0p2\n" if args.case == "d4" else b"ufs:/dev/vtbd0p2\n")
    elif args.case == "d7-send-timeout":
        check("admitted", wait_for(lambda: has_event(ev, "kernel_connected"), 60))
        feed(ctl, b"?\n")
        check("input_sent", wait_for(lambda: has_event(ev, "input_sent", sequence=1), 20,
                                     .01))
        ctl.send_signal(signal.SIGSTOP)
        observations["controller_stopped_seconds"] = args.stop_seconds
        time.sleep(args.stop_seconds)
        ctl.send_signal(signal.SIGCONT)
        observations["kernel_retry_dots"] = serial.text().count(b". ")
        time.sleep(3)
        feed(ctl, b"ufs:/dev/vtbd0p2\n")
    elif args.case == "d7-restart":
        check("admitted", wait_for(lambda: has_event(ev, "kernel_connected"), 60))
        feed(ctl, b"?\n")
        check("listing_before_restart", wait_for(listed, 30))
        ctl.send_signal(signal.SIGKILL)
        ctl.wait()
        observations["first_controller_exit"] = ctl.returncode
        observations["state_after_kill"] = json.loads((base / "controller-state.json")
                                                      .read_text())
        time.sleep(args.stop_seconds)
        ctl = console(args, base, "controller-resumed", resume=True)
        procs.append(ctl)
        ev = base / "controller-resumed-events.jsonl"
        check("resumed", wait_for(lambda: has_event(ev, "controller_resumed"), 10))
        feed(ctl, b"ufs:/dev/vtbd0p2\n")
    elif args.case == "d7-malformed":
        check("admitted", wait_for(lambda: has_event(ev, "kernel_connected"), 60))
        feed(ctl, b"?\n")
        check("listing_before_probes", wait_for(listed, 30))
        probes = [("short", b"x"),
                  ("length_mismatch", HDR.pack(3, 900, 0, 50, 0) + b"abc"),
                  ("unknown_kind", HDR.pack(9, 901, 0, 0, 0)),
                  ("data_without_inner_sequence", HDR.pack(3, 902, 0, 2, 0) + b"ab"),
                  ("data_oversized", HDR.pack(3, 903, 0, 604, 0) + ACK.pack(5000)
                   + b"z" * 600),
                  # Plain mode has no peer authentication: record whether input
                  # from another source port is accepted.
                  ("foreign_port_input", HDR.pack(3, 904, 0, 6, 0) + ACK.pack(2) + b"?\n")]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind((HOST_IP, 0))
            for _, wire in probes:
                probe.sendto(wire, (GUEST_IP, 26004))
                time.sleep(.5)
        observations["probes"] = {name: 900 + i for i, (name, _) in enumerate(probes)}
        time.sleep(2)
        # If the foreign input was accepted it consumed input sequence 2.
        feed(ctl, b"?\nufs:/dev/vtbd0p2\n")
    elif args.case == "d7-link":
        check("admitted", wait_for(lambda: has_event(ev, "kernel_connected"), 60))
        feed(ctl, b"?\n")
        check("listing_before_link_down", wait_for(listed, 30))
        subprocess.run(["sudo", "-n", "ifconfig", args.tap, "down"], check=True)
        observations["link_down_seconds"] = args.stop_seconds
        feed(ctl, b"ufs:/dev/vtbd0p2\n")
        time.sleep(args.stop_seconds)
        subprocess.run(["sudo", "-n", "ifconfig", args.tap, "up"], check=True)
    check("root_mount_attempt", wait_for(mounted, 90))
    try:
        ctl.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass
    final = last_event(ev, "controller_exit")
    observations["controller_exit"] = final
    check("controller_finished", final is not None and final["outcome"] == "finished")
    if args.case in ("d4", "d7-restart", "d7-malformed", "d7-link"):
        check("mountroot_prompt_captured", contains(capture, b"mountroot>"))
        check("disk_list_captured", contains(capture, b"List of GEOM"))
    if args.case == "d7-send-timeout":
        # A timed-out kernel send drops its bytes and reuses its sequence.
        observations["disk_list_captured"] = contains(capture, b"List of GEOM")
        check("kernel_send_timeout_observed",
              final is not None and final["sequence_reuse"] > 0)
    if args.case == "d7-link":
        check("input_retransmitted_while_down",
              final is not None and final["input_retransmit"] > 0)
    if args.case == "d5":
        check("deliberate_panic", wait_for(
            lambda: b"kdb_sysctl_panic" in serial.text() and b"db>" in serial.text(), 60))
        serial.write(f"netgdb -s {HOST_IP} -g {HOST_IP} -c {GUEST_IP} -i vtnet0\r".encode())
        nev = base / "netgdb-events.jsonl"
        check("netgdb_admitted", wait_for(lambda: has_event(nev, "kernel_connected"), 60))
        transcript = []
        try:
            rsp = Rsp((HOST_IP, 20026))
            stop = rsp.packet("?")
            registers = rsp.packet("g")
            threads = [rsp.packet("qfThreadInfo")]
            while threads[-1].startswith("m") and len(threads) < 64:
                threads.append(rsp.packet("qsThreadInfo"))
            address = symbol_address(args.kernel_debug, "version")
            memory = rsp.packet(f"m{address:x},20")
            detach = rsp.packet("D")
            transcript = rsp.transcript
            rsp.close()
            check("stop_reply", stop.startswith(("S", "T")))
            check("registers_read", len(registers) >= 32 and not registers.startswith("E"))
            check("threads_listed", threads[0].startswith("m"))
            check("kernel_memory_read",
                  bytes.fromhex(memory).startswith(b"FreeBSD"))
            check("detached", detach == "OK")
            observations["thread_reply_count"] = len(threads)
            observations["version_bytes"] = bytes.fromhex(memory).decode("latin-1")
        except (OSError, ValueError, LookupError, ConnectionError) as exc:
            observations["rsp_error"] = repr(exc)
            results.setdefault("detached", "fail")
        (base / "rsp-transcript.json").write_text(json.dumps(transcript, indent=1))
        time.sleep(3)
        observations["proxy_exit"] = last_event(nev, "proxy_exit")
        check("post_detach_ddb", wait_for(lambda: serial.text().rstrip().endswith(b"db>"), 20))


def stop_processes(procs):
    for proc in procs:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def udp_payloads(pcap, sport, dport):
    """Yield UDP payloads from an Ethernet/IPv4 pcap between two ports."""
    data = Path(pcap).read_bytes()
    if len(data) < 24:
        return
    order = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
    offset = 24
    while offset + 16 <= len(data):
        incl = struct.unpack_from(order + "I", data, offset + 8)[0]
        frame = data[offset + 16:offset + 16 + incl]
        offset += 16 + incl
        if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
            continue
        ihl = (frame[14] & 15) * 4
        udp = frame[14 + ihl:]
        if len(udp) >= 8 and struct.unpack("!HH", udp[:4]) == (sport, dport):
            yield udp[8:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", choices=CASES, required=True)
    p.add_argument("--base", required=True, help="new evidence directory")
    p.add_argument("--kernel", required=True)
    p.add_argument("--kernel-sha256", required=True)
    p.add_argument("--kernel-debug", help="symbols for d5 memory checks")
    p.add_argument("--loader", default="/boot/loader.efi")
    p.add_argument("--loader-sha256", required=True)
    p.add_argument("--tools", default=str(Path(__file__).parent))
    p.add_argument("--console-sha256", help="required for d4, d5 and d7 cases")
    p.add_argument("--proxy-sha256", help="required for d4, d5 and d7 cases")
    p.add_argument("--loader-lua", help="loader Lua directory built with --loader (D1-D3)")
    p.add_argument("--loader-lua-sha256", help="tree identity of --loader-lua")
    p.add_argument("--tools-sha256", help="tree identity of --tools (D1-D3)")
    p.add_argument("--init", help="static init for d5")
    p.add_argument("--init-sha256")
    p.add_argument("--vm", required=True)
    p.add_argument("--mac", required=True)
    p.add_argument("--nmdm", required=True, help="e.g. /dev/nmdm1160")
    p.add_argument("--boot-id", required=True)
    p.add_argument("--limit", type=int, default=240)
    p.add_argument("--controller-timeout", type=int, default=150)
    p.add_argument("--stop-seconds", type=int, default=40)
    args = p.parse_args()
    if args.case == "d5" and not (args.init and args.kernel_debug):
        p.error("d5 needs --init and --kernel-debug")
    failures, memory, identities = admission(args)
    if failures:
        print(json.dumps({"admitted": False, "failures": failures}), file=sys.stderr)
        return 2
    base = Path(args.base)
    base.mkdir()
    record = {"case": args.case, "vm": args.vm, "mac": args.mac, "boot_id": args.boot_id,
              "limit": args.limit, "admission_memory_bytes": memory,
              "identities": identities, "started": time.time()}
    prepare(args, base)
    record["disk_sha256"] = sha256(base / "disk.img")
    tap = subprocess.run(["sudo", "-n", "ifconfig", "tap", "create"], stdout=subprocess.PIPE,
                         text=True, check=True).stdout.strip()
    args.tap = record["tap"] = tap
    subprocess.run(["sudo", "-n", "ifconfig", tap, "inet", f"{HOST_IP}/24", "up"], check=True)
    tcpdump = subprocess.Popen(["sudo", "-n", "timeout", "-k", "5", str(args.limit + 30),
                                "tcpdump", "-U", "-i", tap, "-s0", "-w",
                                str(base / "traffic.pcap"), "udp"],
                               stdout=open(base / "tcpdump.log", "wb"),
                               stderr=subprocess.STDOUT)
    time.sleep(1)
    serial = Serial(f"{args.nmdm}B", base / "serial.log")
    results, observations, procs = {}, {}, []
    case = run_case(args, base, serial, results, observations, procs)
    next(case)  # controllers listen before the guest starts
    vm_done = threading.Event()
    vm_result = {}

    def guest():
        with open(base / "bhyve.log", "wb") as log:
            status, destroyed = run_bounded_vm(args.vm, [
                "-c", "2", "-m", "1G", "-A", "-H", "-P", "-s", "0:0,hostbridge",
                "-s", "1:0,lpc", "-s", f"2:0,virtio-blk,{base / 'disk.img'}",
                "-s", f"3:0,{'e1000' if args.case == 'd3-early-em' else 'virtio-net'},"
                f"{tap},mac={args.mac}",
                "-l", "bootrom,/usr/local/share/uefi-firmware/BHYVE_UEFI.fd",
                "-l", f"com1,{args.nmdm}A"], args.limit, log=log)
        vm_result.update(status=status, watchdog_destroyed=destroyed)
        vm_done.set()

    threading.Thread(target=guest, daemon=True).start()
    try:
        for _ in case:
            pass
    except Exception as exc:  # the failure is evidence; keep the guest bounded
        observations["case_error"] = repr(exc)
        results["case_completed"] = "fail"
    finally:
        stop_processes(procs)
        record["case_destroy_status"] = destroy_vm(args.vm)
        vm_done.wait(args.limit + 60)
        serial.close()
        tcpdump.wait()
    if args.case == "d7-malformed" and "probes" in observations:
        acked = {ACK.unpack(p)[0] for p in udp_payloads(base / "traffic.pcap", 26004, 26003)
                 if len(p) == ACK.size}
        observations["probe_acked"] = {name: seq in acked
                                       for name, seq in observations["probes"].items()}
        # Only the foreign-port input is well formed; every other probe must be refused.
        results["malformed_probes_refused"] = "pass" if not any(
            v for k, v in observations["probe_acked"].items()
            if k != "foreign_port_input") else "fail"
    record.update(vm=vm_result, results=results, observations=observations,
                  finished=time.time(),
                  passed=bool(results) and all(v == "pass" for v in results.values()))
    (base / "result.json").write_text(json.dumps(record, indent=1, sort_keys=True))
    names = sorted(p.name for p in base.iterdir() if p.is_file() and p.name != "evidence-sha256.txt")
    (base / "evidence-sha256.txt").write_text(
        "".join(f"{sha256(base / n)}  {n}\n" for n in names))
    print(json.dumps({"passed": record["passed"], "results": results, "vm": vm_result}))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
