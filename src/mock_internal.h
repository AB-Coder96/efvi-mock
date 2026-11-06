/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef EFVI_MOCK_INTERNAL_H
#define EFVI_MOCK_INTERNAL_H

/* Internal contract: the caller serializes all operations on an individual VI.
 * Driver/PD/memreg lifetimes and the address registry are mutex-protected. */
#include <etherfabric/vi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define EFVI_MOCK_DEFAULT_RX_CAPACITY 512u
#define EFVI_MOCK_DEFAULT_BUFFER_BYTES 2048u
#define EFVI_MOCK_MAX_RX_CAPACITY 65536u
#define EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES 42u
#define EFVI_MOCK_FILTER_IP4_LOCAL 1u
#define EFVI_MOCK_MAX_FRAME_BYTES UINT16_MAX

struct ef_mock_driver_state;
struct ef_mock_memreg_state;

struct ef_mock_pd_state {
  ef_driver_handle owner;
  unsigned ifindex;
  unsigned children;
  struct ef_mock_driver_state *driver;
};

struct ef_mock_memreg_state {
  ef_driver_handle owner;
  ef_pd *pd;
  uint8_t *base;
  size_t bytes;
  ef_addr address_base;
  unsigned pending;
  struct ef_mock_driver_state *driver;
  struct ef_mock_memreg_state *next;
};

struct ef_mock_rx_descriptor {
  ef_addr address;
  ef_request_id request_id;
  uint8_t *packet;
  size_t buffer_bytes;
  struct ef_mock_memreg_state *registration;
  struct timespec utc;
  struct timespec monotonic;
  uint64_t poll_generation;
  bool timestamp_valid;
};

struct ef_mock_vi_context {
  int socket_fd;
  unsigned ifindex;
  ef_pd *pd;
  ef_driver_handle driver;
  uint32_t rx_capacity;
  uint16_t next_ip_id;
  bool filtered;
  uint32_t filter_addr_be;
  uint16_t filter_port_be;
  struct ef_mock_rx_descriptor *descriptors;
  uint64_t poll_generation;
  uint64_t packets_received;
  uint64_t packets_dropped;
};

/* All functions return negative errno on failure.  The lookup function
 * atomically pins the registration so it cannot be freed while queued. */
int ef_mock_pd_acquire(ef_pd *pd, ef_driver_handle driver);
void ef_mock_pd_release(ef_pd *pd);
int ef_mock_lookup_and_hold(ef_pd *pd, ef_addr addr, size_t size,
                            struct ef_mock_memreg_state **reg,
                            uint8_t **ptr);
void ef_mock_release_registration(struct ef_mock_memreg_state *reg);

#endif
