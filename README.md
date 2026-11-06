# efvi-mock

**Run `ef_vi` receive applications on standard Linux networking.**

`efvi-mock` provides a software implementation of the Solarflare/AMD `ef_vi`
receive programming model. Applications use the familiar driver, protection
 domain, registered memory, descriptor, and event-queue interfaces while the
Linux backend receives Ethernet frames using nonblocking `AF_PACKET` sockets.

It supports IPv4 UDP unicast and multicast traffic, preserves Ethernet frame
contents, and exposes received packets through `ef_eventq_poll()`.

## Features

- **Ethernet-level reception** — `AF_PACKET` / `SOCK_RAW` receives actual
  Ethernet frames, including Ethernet, IPv4, and UDP headers.
- **VLAN-aware receive buffers** — preserves 802.1Q, 802.1ad, and stacked
  VLAN headers, including VLAN ID and priority. When the kernel supplies a
  stripped VLAN header through `PACKET_AUXDATA`, the emulator restores it in
  the receive buffer before delivering the completion event.
- **UDP unicast and multicast** — local IP/port filters and multicast MAC
  membership using the standard `ef_filter_spec` APIs.
- **Application-managed RX descriptors** — memory registration, DMA-style
  addresses, request IDs, receive buffer posting, and ring accounting.
- **Nonblocking event polling** — RX, truncated-frame, and no-descriptor
  events through `ef_eventq_poll()`.
- **Software receive timestamps** — nanosecond-resolution realtime clock
  readings exposed through the receive timestamp interfaces.
- **C and C++ development** — public interface available as
  `<etherfabric/vi.h>` and a CMake target named `efvi::mock`.

## Build

Requires Linux, CMake 3.16+, and a C11 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Raw Ethernet sockets require `CAP_NET_RAW`. Run the example with `sudo`, or
assign the capability to the executable:

```bash
sudo setcap cap_net_raw+ep ./build/efvi_udp_monitor
```

## Run the receiver

Receive IPv4 UDP traffic addressed to `127.0.0.1:9000` on loopback:

```bash
sudo ./build/efvi_udp_monitor 127.0.0.1 9000 lo
```

Send a packet from another terminal:

```bash
python3 -c 'import socket; socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(b"hello", ("127.0.0.1", 9000))'
```

To receive a UDP multicast group on an Ethernet interface:

```bash
sudo ./build/efvi_udp_monitor 239.42.0.1 9000 eth0
```

The optional interface argument selects the protection-domain interface;
without it, the socket observes all interfaces. For multicast, the emulator
adds packet-socket multicast MAC membership on the selected interface, or
on available interfaces when no interface is selected. Network infrastructure
using IGMP snooping may additionally require an active IGMP membership to
forward traffic to the host.

## Ethernet and VLAN layout

Each `EF_EVENT_TYPE_RX` completion identifies a posted receive buffer.
The bytes in that buffer begin with the received Ethernet header:

```text
Untagged Ethernet:
  [destination MAC:6][source MAC:6][EtherType:2][L3 packet ...]

802.1Q:
  [destination MAC:6][source MAC:6][TPID=0x8100:2]
  [TCI:2][inner EtherType:2][L3 packet ...]

802.1ad / QinQ:
  [destination MAC:6][source MAC:6][TPID=0x88a8:2]
  [outer TCI:2][TPID=0x8100:2][inner TCI:2]
  [inner EtherType:2][L3 packet ...]
```

The 16-bit tag control information (TCI) includes PCP (bits 15–13), DEI
(bit 12), and VLAN ID (bits 11–0). The sample receiver prints the VLAN ID,
priority, and tag count when a VLAN-tagged frame is received.

The application obtains the frame length from `EF_EVENT_RX_BYTES(event)`
and the matching buffer from `EF_EVENT_RX_RQ_ID(event)`. `ef_vi` does not
require an additional VLAN field in the completion event: VLAN tags are
part of the Ethernet frame bytes.

If the Linux NIC driver has already removed the outer VLAN tag, the emulator
uses `PACKET_AUXDATA` metadata to insert the tag back into the frame before
reporting the RX event. This is included in the reported frame length and
must fit in the posted receive buffer.

## Integration

Include the public header:

```c
#include <etherfabric/vi.h>
```

Link using CMake:

```cmake
add_subdirectory(path/to/efvi-mock)
target_link_libraries(your_application PRIVATE efvi::mock)
```

A typical receive flow is:

1. `ef_driver_open()` → `ef_pd_alloc()`.
2. Register page-aligned buffer memory with `ef_memreg_alloc()`.
3. Create the virtual interface with `ef_vi_alloc_from_pd()`.
4. Configure an IPv4 UDP destination filter using
   `ef_filter_spec_set_ip4_local()` and `ef_vi_filter_add()`.
5. Post RX buffers using `ef_vi_receive_init()` and `ef_vi_receive_push()`.
6. Poll `ef_eventq_poll()`, inspect each Ethernet frame, and repost the
   completed buffer with `ef_vi_receive_post()`.
7. Free resources in reverse order.

The example in [`examples/udp_monitor.c`](examples/udp_monitor.c) demonstrates
the complete receive loop.

### Supported API

| Category | Interfaces |
| --- | --- |
| Driver | `ef_driver_open`, `ef_driver_close` |
| Protection domain | `ef_pd_alloc`, `ef_pd_free` |
| Memory | `ef_memreg_alloc`, `ef_memreg_free`, `ef_memreg_dma_addr` |
| Virtual interface | `ef_vi_alloc_from_pd`, `ef_vi_free` |
| Filtering | `ef_filter_spec_init`, `ef_filter_spec_set_ip4_local`, `ef_vi_filter_add` |
| Receive descriptors | `ef_vi_receive_init`, `ef_vi_receive_push`, `ef_vi_receive_post` |
| Receive ring | `ef_vi_receive_capacity`, `ef_vi_receive_fill_level`, `ef_vi_receive_space` |
| Event queue | `ef_eventq_poll`, `EF_EVENT_TYPE_RX`, `EF_EVENT_TYPE_RX_DISCARD` |
| Timestamps | `ef_vi_receive_get_timestamp`, `ef_vi_receive_get_timestamp_with_sync_flags`, `ef_vi_receive_get_precise_timestamp` |

## Replaying Ethernet frames

The emulator accepts any IPv4 UDP payload format, including binary data. For
Ethernet-accurate replay, including VLAN tags, replay a pcap onto an interface
connected to the receiving interface (for example, a veth pair or separate
host):

```bash
sudo tcpreplay --intf1=<transmit-interface> capture.pcap
```

A UDP-payload replay helper is also included in `tools/pcap_to_udp.py` for
testing application-level traffic over the ordinary Linux UDP stack.

## Development and tests

```bash
ctest --test-dir build --output-on-failure
```

The suite includes header checks, VLAN packet parsing and restoration tests,
a raw-socket integration test, and a PCAP parser test. The live receive
integration test runs when `CAP_NET_RAW` is available.

## License

BSD-2-Clause. See [LICENSE](LICENSE).

`efvi-mock` is an independent software implementation and is not affiliated
with or endorsed by AMD or Solarflare.
