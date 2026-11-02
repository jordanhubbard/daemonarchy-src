#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Run an original unittest directory (discovery, as CI does) and write a literate-ai retained test report.

Report producer only: it records the outcome each case actually has when this
command runs.  It never fabricates, defaults or reuses an outcome.  Exit status is
nonzero when any case failed, after the report has been written.
"""
import argparse
import json
import sys
import unittest
from pathlib import Path


class Recorder(unittest.TestResult):
    def __init__(self):
        super().__init__()
        self.outcomes = {}

    def _set(self, test, outcome, reason=None):
        # A failure is never downgraded by a later event for the same case.
        if self.outcomes.get(test.id(), ("passed", None))[0] == "failed":
            return
        self.outcomes[test.id()] = (outcome, reason)

    def addSuccess(self, test):
        self._set(test, "passed")

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._set(test, "failed")

    def addError(self, test, err):
        super().addError(test, err)
        self._set(test, "failed")

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._set(test, "skipped", reason or "skipped without a stated reason")

    def addExpectedFailure(self, test, err):
        super().addExpectedFailure(test, err)
        self._set(test, "passed")

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self._set(test, "failed")

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            self._set(test, "failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--test-dir", required=True)
    ap.add_argument("--pattern", default="test_*.py")
    ap.add_argument("--output", required=True)
    ap.add_argument("--action", default="draft-controller-tests")
    args = ap.parse_args()
    start = Path(args.test_dir).resolve(strict=True)
    sys.path.insert(0, str(start))
    suite = unittest.defaultTestLoader.discover(
        str(start), pattern=args.pattern, top_level_dir=str(start))
    suite.run(recorder)
    for test, _ in recorder.errors + recorder.failures:
        recorder._set(test, "failed")
    cases = [
        {"id": name, "outcome": outcome, "reason": reason}
        for name, (outcome, reason) in sorted(recorder.outcomes.items())
    ]
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        "schema": "literate-ai/retained-project-test-report@1",
        "action": args.action,
        "cases": cases,
    }, indent=2) + "\n")
    return 1 if any(c["outcome"] == "failed" for c in cases) else 0


if __name__ == "__main__":
    sys.exit(main())
