# Utility interface

Preserve the original `truncate(1)` CLI and exit/error behavior, including file
creation/no-create, absolute and relative sizing, rounding, reference sizing,
offset/length deallocation and multiple operands. The original source and manual
define option interactions. Consume the locked parser slice through the exact
shared header; do not rely on ambient libutil resolution.
