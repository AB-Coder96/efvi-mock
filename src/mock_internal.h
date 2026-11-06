/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef EFVI_MOCK_INTERNAL_H
#define EFVI_MOCK_INTERNAL_H

/* Never install this header: backend state is deliberately private. */
#include <etherfabric/vi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define EFVI_MOCK_DEFAULT_RX_CAPACITY 512u
#define EFVI_MOCK_DEFAULT_BUFFER_BYTES 2048u
#define EFVI_MOCK_MAX_RX_CAPACITY 65536u
#define EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES 42u

enum ef_mock_vi_lifecycle {
  EF_MOCK_VI_NEW = 0,
  EF_MOCK_VI_ALLOCATED,
  EF_MOCK_VI_FILTERED,
  EF_MOCK_VI_CLOSED
};

struct ef_mock_pd_state {
  ef_driver_handle owner;
  unsigned ifindex;
};

struct ef_mock_memreg_state {
  ef_driver_handle owner;
  ef_pd *pd;
  uint8_t *base;
  size_t bytes;
  ef_addr address_base;
};

struct ef_mock_rx_descriptor {
  ef_addr address;
  ef_request_id request_id;
  void *packet;
  size_t buffer_bytes;
  struct timespec utc;
  struct timespec monotonic;
  uint64_t poll_generation;
  bool timestamp_valid;
};

/*
 * Timestamp metadata stays with the descriptor that produced an event.
 * Only packets completed in the most recent poll generation are queryable.
 * Polling again invalidates timestamps from the previous generation.
 */

struct ef_mock_vi_context {
  enum ef_mock_vi_lifecycle lifecycle;
  int socket_fd;
  unsigned ifindex;
  uint32_t rx_capacity;
  struct ef_mock_rx_descriptor *descriptors;
  uint64_t poll_generation;
  uint64_t packets_received;
  uint64_t packets_dropped;
};

#endif /* EFVI_MOCK_INTERNAL_H */
