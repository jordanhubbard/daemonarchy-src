# Utility acceptance

Run the complete twenty-case original ATF shell inventory against newly built
outputs. Preserve the `deallocate` case's root requirement and execute that case
in a disposable privileged acceptance VM. An unprivileged skip is incomplete
coverage and cannot become a passing suite.
The `bad_truncate` and `cannot_open` cases explicitly require an unprivileged
user. Preserve their required-user metadata too: running every case as root
does not qualify permission-error behavior. Bind the runner's user selection
and any privilege switching to the reviewed disposable test environment.

Independently exercise sealed products with private fixtures. Observe file
lengths, content-prefix preservation, extension contents, no-create behavior,
relative/reference/rounding results and invalid-input refusal. A zero exit code
without the expected file effects fails acceptance. Verify reopened pkg payloads
and dependency metadata, then relocated installation/update/rollback.

Measure unchanged reuse and separate source, parser dependency, shared header,
options and compiler changes. Require the actual affected action closure and
reject stale plans, receipts and packages.
