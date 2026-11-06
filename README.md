# efvi-mock

A hardware-free Linux implementation of a **subset** of the AMD/Solarflare
`ef_vi` userspace interface for CI, WSL2, and generic VMs.

## Compatibility contract

- **Source compatibility:** target common receive-path calls and event macros.
- **Binary compatibility:** **not** supported. Rebuild applications against
  this header and link them to the mock backend. Never mix real SDK and mock
  objects in a single binary.
- Public `ef_pd`, `ef_memreg`, and `ef_vi` types must be concrete: conventional
  SDK applications create these as local variables. Their emulator storage is
  private behind `mock_private` pointers.
- `ef_timespec` follows the SDK's userspace `struct timespec` spelling.
- UTC-style RX timestamps will use `CLOCK_REALTIME`; `CLOCK_MONOTONIC` will
  be used for elapsed-time metrics. Software time is **not** NIC/PTP time.
- RX data will represent Ethernet/IP/UDP frames, not just the UDP payload.
  The eventual POSIX UDP implementation must synthesize missing L2/L3/L4
  headers from `recvmsg` metadata; it cannot claim wire-identical packets.
- `recvmsg` into application-owned registered buffers is **not** kernel bypass
  or true DMA zero-copy. No synthetic latency measurement represents NIC
  hardware latency.
- This first version targets the conventional posted-buffer RX model. X3/EFCT,
  TX, and packed-stream features are intentionally out of scope.
- Only one polling thread may access a VI at a time unless later documented.

## Progress

| Commit | State | Scope |
|---|---|---|
| 1 | Included | Public declarations, private state skeleton, compile-time smoke test |
| 2 | Planned | Driver, PD, memory registration lifecycle and validation |
| 3 | Planned | Non-blocking UDP, filter provisioning, RX descriptor semantics |
| 4 | Planned | RX polling, events, timestamp extraction, overflow policies |
| 5 | Planned | CMake, example receiver, CI, integration and sanitizer testing |

## Commit 1 verification

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  -Iinclude tests/header_smoke.c -o /tmp/efvi-header-smoke
/tmp/efvi-header-smoke
```

This commit exposes declarations for later implementation: linking a program
which calls driver/VI/event functions is expected to fail until later commits.

## Reference

Public signatures and conventions were checked against the BSD-2-Clause
Etherfabric headers in the AMD/Xilinx OpenOnload repository:
https://github.com/Xilinx-CNS/onload/tree/master/src/include/etherfabric

No vendor implementations are included in this project.
