"""Last-stage retention for the network debug controllers.

A hard reset is silence: the target sends nothing.  The controller can
only report the last stage it actually received, so this record is
rewritten atomically whenever the stage changes and finished with the
controller's outcome.  It records panic text only when panic text was
received; silence is never reported as a panic or a reset cause.
"""

import json
import os
import re
import time

SCHEMA = "daemonarchy-netdebug-last-stage@1"
MARKER = re.compile(rb"netdebug-stage: ([a-z0-9-]+)([^\r\n]*)")
PANIC = re.compile(rb"panic: ([^\r\n]{0,200})")
# Ordered milestones recognised in console output.
MILESTONES = ((b"Trying to mount root from", "root_mount_attempt"),
              (b"mountroot>", "mountroot_prompt"),
              (b"KDB: enter: panic", "debugger_entered"),
              (b"db>", "ddb_prompt"))
TAIL = 512


class StageRecorder:
    def __init__(self, path, boot_id=None, *, resume=False):
        self.path = path
        if resume and path is not None and os.path.exists(path):
            with open(path, encoding="utf-8") as handle:
                previous = json.load(handle)
            if previous.get("schema") == SCHEMA and previous.get("boot_id") == boot_id:
                previous.update(outcome=None, conclusion=None)
                previous.setdefault("restarts", 0)
                previous["restarts"] += 1
                self.state = previous
                self._pending = b""
                self._write()
                return
        self.state = {"schema": SCHEMA, "boot_id": boot_id, "stage": "listening",
                      "stage_time": time.time(), "earliest_stage": None,
                      "markers": [], "milestones": [], "panic_text": None,
                      "output_tail": "", "outcome": None, "conclusion": None}
        self._pending = b""
        self._write()

    def _write(self):
        if self.path is None:
            return
        tmp = f"{self.path}.tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(self.state, handle, sort_keys=True)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, self.path)

    def stage(self, name, **fields):
        self.state.update(stage=name, stage_time=time.time(), **fields)
        self._write()

    def output(self, data):
        """Record markers, milestones and panic text from received bytes."""
        text = self._pending + bytes(data)
        # Keep an incomplete trailing line for the next chunk.
        cut = max(text.rfind(b"\n"), text.rfind(b"\r"))
        complete, self._pending = (b"", text) if cut < 0 else (text[:cut + 1], text[cut + 1:])
        if len(self._pending) > TAIL:
            complete, self._pending = complete + self._pending, b""
        changed = False
        seen = []  # (position, stage) in received order
        for match in MARKER.finditer(complete):
            name = match.group(1).decode("ascii")
            fields = dict(part.split("=", 1) for part in
                          match.group(2).decode("ascii", "replace").split() if "=" in part)
            self.state["markers"].append({"stage": name, **fields, "time": time.time()})
            if self.state["earliest_stage"] is None:
                self.state["earliest_stage"] = name
            seen.append((match.start(), name))
        window = complete + self._pending
        for needle, name in MILESTONES:
            position = window.rfind(needle)
            if position >= 0:
                if name not in self.state["milestones"]:
                    self.state["milestones"].append(name)
                seen.append((position, name))
        if seen:
            self.state.update(stage=max(seen)[1], stage_time=time.time())
            changed = True
        panic = PANIC.search(complete)
        if panic and self.state["panic_text"] is None:
            self.state["panic_text"] = panic.group(1).decode("ascii", "replace")
            changed = True
        tail = (self.state["output_tail"].encode("latin-1") + bytes(data))[-TAIL:]
        self.state["output_tail"] = tail.decode("latin-1")
        if changed:
            self._write()

    def finish(self, outcome):
        state = self.state
        state["outcome"] = outcome
        if outcome in ("finished", "kernel_transport_closed"):
            state["conclusion"] = "transport closed by the target after stage " + state["stage"]
        elif state["panic_text"] is not None:
            state["conclusion"] = "panic text received: " + state["panic_text"]
        else:
            state["conclusion"] = ("no data after stage " + state["stage"] +
                                   "; silence does not establish a panic or reset cause")
        state["finish_time"] = time.time()
        self._write()
        return state
