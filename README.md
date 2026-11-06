# efvi-mock

A Linux, WSL2, and CI-friendly **source-compatible subset** of the
Solarflare/AMD `ef_vi` receive API. This project is an independent mock
implementation, not a fork, real NIC simulator, or hardware bypass library.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/efvi_udp_monitor 127.0.0.1 26477
# In another terminal: printf 'hello' | socat - UDP:127.0.0.1:26477
```

For multicast, use `./build/efvi_udp_monitor 239.192.1.10 26477` and
send a multicast UDP datagram to that group/port on an interface with
multicast support. A raw `tcpreplay` frame injection is not guaranteed to
reach a normal Linux UDP socket, depending on routing, namespaces, and NIC.

## API coverage

- `ef_driver_open`, `ef_driver_close`
- `ef_pd_alloc`, `ef_pd_free`
- `ef_memreg_alloc`, `ef_memreg_free`, `ef_memreg_dma_addr`
- `ef_vi_alloc_from_pd`, `ef_vi_free`
- `ef_filter_spec_init`, `ef_filter_spec_set_ip4_local`, `ef_vi_filter_add`
- `ef_vi_receive_init`, `ef_vi_receive_push`, `ef_vi_receive_post`
- `ef_eventq_poll` and `EF_EVENT_TYPE_RX`, `EF_EVENT_TYPE_RX_DISCARD`,
  `EF_EVENT_TYPE_RX_NO_DESC_TRUNC`
- `ef_vi_receive_get_timestamp`,
  `ef_vi_receive_get_timestamp_with_sync_flags`,
  `ef_vi_receive_get_precise_timestamp`

All APIs return 0 or a negative errno except event polling, which returns
an event count. The mock deliberately rejects unsupported TX, packed stream,
EFCT, physical addressing, non-UDP filters, and shared event queues with
`-EOPNOTSUPP` rather than pretending to implement hardware-specific features.

## Compatibility / limitations

**Source, not ABI, compatibility.** Applications must rebuild using this
project's `include/etherfabric/vi.h`. A shared library drop-in for the real
SDK is not supported. Only the explicitly listed receive-path subset is
implemented; structure layouts do not match vendor ABI.

**Not a kernel bypass or zero-copy receive.** The kernel receives UDP and
`recvmsg()` copies its UDP payload directly into a registered application
buffer. The emulator **synthesizes** 14-byte Ethernet, 20-byte IPv4, and
8-byte UDP headers around that payload in-place. Consequently, the generated
Ethernet frame is not byte-for-byte equivalent to an original wire packet:
MACs, IP TTL, ID, DF, IPv4 checksum are synthetic and IPv4 UDP checksum is
zero (which is valid for IPv4). Linux handles IP fragmentation/reassembly.

**No hardware timestamps.** `CLOCK_REALTIME` is sampled after `recvmsg()`;
`CLOCK_MONOTONIC` is also sampled for internal metrics. The timestamp sync
flags and fractional nanoseconds are zero because NIC/PTP precision cannot
be claimed. Timestamps remain queryable only until the next `ef_eventq_poll`
on the same VI, and only for packets completed in the most recent poll.

**Resource and ring rules.** Zero-initialize `ef_pd`, `ef_memreg`, and
`ef_vi` before allocation; use `ef_driver_open` for a valid handle. Register
page-aligned memory, and provide 64-byte-aligned offsets to
`ef_vi_receive_init` / `post`. Buffers must be sized for full synthetic
Ethernet packets (`ef_vi_receive_buffer_len()` defaults to 2048). A posted
buffer is released from the ring when its completion is polled; then it can
be reused or reposted. Memory registrations and PDs return `-EBUSY` while
referenced by live resources. `ef_vi_receive_push()` publishes previously
staged descriptors. Ring capacity must be a power of two; a single VI is
single-threaded. No user-level allocation occurs on the RX polling path.

**UDP filtering.** Only one IPv4 UDP local-address/port filter per VI is
supported. Multicast filters join the requested group, and a PD with
ifindex 0 accepts the host's default multicast routing/interface selection.
Binding and joining can still fail under container/WSL2 multicast restrictions.
No binding to an arbitrary network device or reuse across independent VIs is
promised. The normal socket receive queue is subject to kernel UDP drops;
those drops do not necessarily generate mock events. A datagram arriving
while the VI has no pushed descriptor generates `RX_NO_DESC_TRUNC` when
polled. Oversize datagrams generate `RX_DISCARD`.

**Clean shutdown.** First free the VI, then free the memreg, PD and driver.
A memreg cannot be freed while pending RX descriptors reference it. Since
this is a mock backend, it does not pin the memory or program an IOMMU.

## References

AMD/Xilinx OpenOnload ef_vi public API:
https://github.com/Xilinx-CNS/onload/tree/master/src/include/etherfabric

The implementation and public header layouts here are independently written
under BSD-2-Clause. It is not affiliated with AMD or Solarflare.

## Replay an existing market-data PCAP without real NIC hardware

Run the monitor (one terminal), then replay Ethernet/IP/UDP PCAP payloads to
its UDP socket (another terminal). Multicast packets are redirected to
loopback by default, so WSL2/cloud VMs need no multicast setup:

```sh
./build/efvi_udp_monitor 127.0.0.1 26477
python3 tools/pcap_to_udp.py market_data.pcap --host 127.0.0.1 --port 26477
```

Use `--speed 0` to flood as fast as possible, or `--speed 2` for 2x original
capture rate. This tool is intentionally *not* a byte-perfect wire replayer:
only complete, unfragmented IPv4 UDP payloads are sent through POSIX sockets.
