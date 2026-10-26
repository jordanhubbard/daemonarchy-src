# Panic NetGDB proxy prototype

`netgdb-proxy.py` bridges FreeBSD NetGDB debugnet packets to a TCP GDB
remote session on a trusted local link. It binds UDP 20025 and TCP 20026,
requires the expected kernel IPv4 address and NetGDB v1 herald, acknowledges
and de-duplicates kernel packets, and retransmits unacknowledged GDB input
within a finite deadline. The binary capture contains kernel-to-GDB bytes;
the JSONL events record session stages. Both outputs must use new private paths.

```
python3 tools/daemonarchy/netgdb-proxy.py --bind 192.0.2.1 \
  --peer 192.0.2.2 --timeout 120 \
  --capture /path/to/new-kernel-gdb.bin \
  --events /path/to/new-kernel-gdb-events.jsonl
```

After a deliberate **later** kernel panic and `netgdb -s 192.0.2.1 -c
192.0.2.2 -g 192.0.2.1 -i vtnet0` in DDB, connect GDB with
`target remote 192.0.2.1:20026`. NetGDB currently requires panic state and a
debugnet-capable attached native NIC. This prototype uses plain packets and
provides neither authentication nor confidentiality. It does not activate
itself at panic, cover a pre-driver panic, or inherit the earlier firmware
boot ID. Native VM panic and GDB tests are required before claiming those
stages; the TCP/UDP loopback test covers only proxy packet handling.
