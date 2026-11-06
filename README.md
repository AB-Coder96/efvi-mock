# efvi-mock

**Run applications built around Solarflare's `ef_vi` receive API on standard Linux systems.**

`efvi-mock` is an open-source software implementation of the core `ef_vi` receive programming model. It provides familiar driver, protection-domain, memory-registration, receive-descriptor, and event-queue interfaces, backed by Linux raw packet sockets.

Use it as a C or C++ library in applications that need Ethernet-level packet reception without a Solarflare network adapter.

## Features

- **Raw Ethernet reception:** `AF_PACKET` / `SOCK_RAW` delivers Ethernet frames into application-posted receive buffers.
- **UDP unicast and multicast:** IPv4 UDP destination filtering and multicast reception through the `ef_vi`-style filter API.
- **VLAN-aware frames:** 802.1Q, 802.1ad, and stacked VLAN headers are represented in received frame bytes, with `PACKET_AUXDATA` handling for stripped outer tags.
- **Receive rings:** Registered buffers, DMA-style address tokens, receive request IDs, posting, and ring accounting.
- **Event polling:** Nonblocking packet completions through `ef_eventq_poll()` and `ef_event` structures.
- **Software receive timestamps:** Nanosecond-resolution timestamps through the supported receive timestamp interfaces.
- **C and C++ integration:** Include `<etherfabric/vi.h>` and link the `efvi::mock` CMake target.

## Requirements

- Linux with `AF_PACKET` support
- CMake 3.16 or newer
- C11-compatible compiler (C++ applications are supported)
- `CAP_NET_RAW` permission for applications opening raw packet sockets

## Install

Clone and install the library into a user-local prefix:

```bash

cd efvi-mock

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DBUILD_TESTING=OFF \
  -DEFVI_MOCK_BUILD_EXAMPLES=OFF

cmake --build build --parallel
cmake --install build
```

This installs the public `etherfabric/vi.h` header, the `efvi_mock` library, and the CMake package configuration.

To build a shared library instead of a static library, set `-DEFVI_MOCK_SHARED=ON` when configuring.

## Use in your application

Include the public API header in your C or C++ source:

```cpp
#include <etherfabric/vi.h>

int main() {
    ef_driver_handle driver;
    if (ef_driver_open(&driver) != 0)
        return 1;

    // Allocate a protection domain, register buffers,
    // post receive descriptors, and poll ef_eventq_poll().

    return ef_driver_close(driver) == 0 ? 0 : 1;
}
```

In your application's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_receiver LANGUAGES CXX)

find_package(efvi_mock CONFIG REQUIRED)

add_executable(my_receiver main.cpp)
target_link_libraries(my_receiver PRIVATE efvi::mock)
```

Configure and build your application using the installation prefix:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build --parallel
```

Once the package is installed in a standard system prefix, `CMAKE_PREFIX_PATH` generally is not needed.

### Run your application

Linux raw packet sockets require `CAP_NET_RAW`. For a local development executable:

```bash
sudo ./build/my_receiver
```

Alternatively, grant the capability to the executable:

```bash
sudo setcap cap_net_raw+ep ./build/my_receiver
./build/my_receiver
```

## Receive model

The application allocates an `ef_vi` context, registers receive memory, posts descriptors, and polls for completions. Each completed receive buffer contains a raw Ethernet frame and is identified by its receive request ID.

```text
Ethernet interface
       |
       v
Linux AF_PACKET / SOCK_RAW
       |
       v
Registered receive buffers
       |
       v
ef_eventq_poll() --> ef_event --> application
```

Typical API sequence:

1. `ef_driver_open()` and `ef_pd_alloc()` to initialize the driver and protection domain.
2. `ef_memreg_alloc()` to register the application's receive memory.
3. `ef_vi_alloc_from_pd()` to create the virtual interface.
4. `ef_filter_spec_set_ip4_local()` and `ef_vi_filter_add()` to select IPv4 UDP traffic.
5. `ef_vi_receive_init()` and `ef_vi_receive_push()` to post receive buffers.
6. `ef_eventq_poll()` to process completions; then repost consumed buffers with `ef_vi_receive_post()`.
7. Release resources in reverse order.

### Ethernet and VLAN frames

The returned buffer begins with the Ethernet header. VLAN tags, when present, are part of the frame bytes rather than separate event fields:

```text
Untagged: [Dst MAC][Src MAC][EtherType][Payload]
802.1Q:   [Dst MAC][Src MAC][0x8100][TCI][EtherType][Payload]
802.1ad:  [Dst MAC][Src MAC][0x88A8][Outer TCI][0x8100][Inner TCI][EtherType][Payload]
```

The VLAN tag control information (TCI) contains priority (PCP), drop eligibility (DEI), and VLAN ID. The receive event provides the request ID and received frame length through `EF_EVENT_RX_RQ_ID(event)` and `EF_EVENT_RX_BYTES(event)`.

## Example receiver

Build the included receiver example:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo ./build/efvi_udp_monitor 239.42.0.1 9000 eth0
```

The final argument selects the interface. The receiver prints packet details and VLAN information when present.

## Supported API

| Area | Functions and interfaces |
| --- | --- |
| Driver | `ef_driver_open`, `ef_driver_close` |
| Protection domain | `ef_pd_alloc`, `ef_pd_free` |
| Memory | `ef_memreg_alloc`, `ef_memreg_free`, `ef_memreg_dma_addr` |
| Virtual interface | `ef_vi_alloc_from_pd`, `ef_vi_free` |
| Filtering | `ef_filter_spec_init`, `ef_filter_spec_set_ip4_local`, `ef_vi_filter_add` |
| Receive descriptors | `ef_vi_receive_init`, `ef_vi_receive_push`, `ef_vi_receive_post` |
| Ring state | `ef_vi_receive_capacity`, `ef_vi_receive_fill_level`, `ef_vi_receive_space` |
| Events | `ef_eventq_poll`, `EF_EVENT_TYPE_RX`, `EF_EVENT_TYPE_RX_DISCARD` |
| Timestamps | `ef_vi_receive_get_timestamp`, `ef_vi_receive_get_timestamp_with_sync_flags`, `ef_vi_receive_get_precise_timestamp` |

## License

BSD-2-Clause. See [LICENSE](LICENSE).

`efvi-mock` is an independent implementation and is not affiliated with or endorsed by AMD or Solarflare.
