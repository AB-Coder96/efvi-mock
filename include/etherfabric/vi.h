/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef EFVI_MOCK_ETHERFABRIC_VI_H
#define EFVI_MOCK_ETHERFABRIC_VI_H

/*
 * Hardware-free, source-compatible subset of the ef_vi userspace API.
 * This header is an independent implementation, NOT the vendor ABI.
 * Public structs are concrete because ef_vi applications stack-allocate them.
 * APIs without an implementation are intentional until subsequent commits.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EFVI_MOCK_API_VERSION 1u
#define EF_VI_NIC_PAGE_SHIFT 12u
#define EF_VI_NIC_PAGE_SIZE (1u << EF_VI_NIC_PAGE_SHIFT)
#define EF_VI_NIC_PAGE_MASK (EF_VI_NIC_PAGE_SIZE - 1u)
#define EF_VI_DMA_ALIGN 64u
#define EF_VI_EVENT_POLL_MIN_EVS 2
#define EF_VI_MAX_QS 32
#define EF_REQUEST_ID_MASK UINT32_MAX
#define EF_VI_ALIGN(n) __attribute__((aligned(n)))

#define ef_vi_inline static inline

/* This is the userspace spelling used by the SDK. */
#define ef_timespec struct timespec

typedef int ef_driver_handle;
typedef int ef_request_id;
typedef uint64_t ef_addr;
typedef uint32_t ef_eventq_ptr;
typedef char *ef_vi_ioaddr_t;

typedef struct {
  int64_t tv_sec;
  uint32_t tv_nsec;
  uint16_t tv_nsec_frac;
  uint16_t tv_flags;
} ef_precisetime;

/* Only the supported receive-oriented feature flags are advertised. */
enum ef_vi_flags {
  EF_VI_FLAGS_DEFAULT = 0,
  EF_VI_RX_TIMESTAMPS = 0x10000
};

enum ef_pd_flags {
  EF_PD_DEFAULT = 0,
  EF_PD_VF = 0x1,
  EF_PD_PHYS_MODE = 0x2,
  EF_PD_RX_PACKED_STREAM = 0x4,
  EF_PD_MCAST_LOOP = 0x10
};

/* These structures have stable mock-local layouts, not vendor layouts. */
typedef struct ef_pd {
  enum ef_pd_flags pd_flags;
  unsigned pd_resource_id;
  char *pd_intf_name;
  void *mock_private;
} ef_pd;

typedef struct ef_memreg {
  ef_addr *mr_dma_addrs;
  ef_addr *mr_dma_addrs_base;
  void *mock_private;
} ef_memreg;

/* RX event values match the documented conventional event enumerators. */
enum {
  EF_EVENT_TYPE_RX = 0,
  EF_EVENT_TYPE_TX,
  EF_EVENT_TYPE_RX_DISCARD,
  EF_EVENT_TYPE_TX_ERROR,
  EF_EVENT_TYPE_RX_NO_DESC_TRUNC,
  EF_EVENT_TYPE_SW,
  EF_EVENT_TYPE_OFLOW,
  EF_EVENT_TYPE_TX_WITH_TIMESTAMP,
  EF_EVENT_TYPE_RX_PACKED_STREAM,
  EF_EVENT_TYPE_RX_MULTI,
  EF_EVENT_TYPE_TX_ALT,
  EF_EVENT_TYPE_RX_MULTI_DISCARD,
  EF_EVENT_TYPE_RESET,
  EF_EVENT_TYPE_RX_MULTI_PKTS,
  EF_EVENT_TYPE_RX_REF,
  EF_EVENT_TYPE_RX_REF_DISCARD,
  EF_EVENT_TYPE_MAX
};

enum {
  EF_EVENT_FLAG_SOP = 0x1,
  EF_EVENT_FLAG_CONT = 0x2,
  EF_EVENT_FLAG_ISCSI_OK = 0x4,
  EF_EVENT_FLAG_MULTICAST = 0x8
};

enum {
  EF_VI_SYNC_FLAG_CLOCK_SET = 0x1,
  EF_VI_SYNC_FLAG_CLOCK_IN_SYNC = 0x2
};

/*
 * C bitfields in the real SDK are compiler/architecture dependent.
 * This mock intentionally uses an independent, simple event representation.
 */
typedef union ef_event {
  struct {
    uint16_t type;
  } generic;
  struct {
    uint16_t type;
    uint8_t q_id;
    uint8_t reserved;
    uint32_t rq_id;
    uint16_t len;
    uint16_t flags;
    uint16_t ofs;
  } rx;
  struct {
    uint16_t type;
    uint8_t q_id;
    uint8_t reserved;
    uint32_t rq_id;
    uint16_t len;
    uint16_t flags;
    uint16_t subtype;
  } rx_discard;
  struct {
    uint16_t type;
    uint8_t q_id;
    uint8_t reserved;
  } rx_no_desc_trunc;
  struct {
    uint16_t type;
    uint16_t reserved;
    uint32_t data;
  } sw;
} ef_event;

#define EF_EVENT_TYPE(ev)              ((ev).generic.type)
#define EF_EVENT_RX_BYTES(ev)          ((ev).rx.len)
#define EF_EVENT_RX_Q_ID(ev)           ((ev).rx.q_id)
#define EF_EVENT_RX_RQ_ID(ev)          ((ev).rx.rq_id)
#define EF_EVENT_RX_CONT(ev)           ((ev).rx.flags & EF_EVENT_FLAG_CONT)
#define EF_EVENT_RX_SOP(ev)            ((ev).rx.flags & EF_EVENT_FLAG_SOP)
#define EF_EVENT_RX_DISCARD_TYPE(ev)   ((ev).rx_discard.subtype)

/* Internal ring counters follow unsigned modular arithmetic. */
typedef struct ef_vi_rxq_state {
  uint32_t posted;   /* Visible to the simulated device. */
  uint32_t added;    /* Accepted descriptors, including staged entries. */
  uint32_t removed;  /* Descriptors consumed by receive completions. */
} ef_vi_rxq_state;

typedef struct ef_eventq_state {
  ef_eventq_ptr evq_ptr;
} ef_eventq_state;

typedef struct ef_vi_state {
  ef_eventq_state evq;
  ef_vi_rxq_state rxq;
} ef_vi_state;

typedef struct ef_vi_rxq {
  uint32_t mask;  /* A power-of-two capacity minus one. */
  void *descriptors;
  uint32_t *ids;
} ef_vi_rxq;

typedef struct ef_vi {
  ef_vi_rxq vi_rxq;
  ef_vi_state *ep_state;
  enum ef_vi_flags vi_flags;
  unsigned rx_buffer_len;
  unsigned rx_prefix_len;
  void *mock_private;  /* Owned by the emulator; never by applications. */
} ef_vi;

/* A source-compatible subset of the existing filter API. */
enum ef_filter_flags { EF_FILTER_FLAG_NONE = 0 };

typedef struct ef_filter_spec {
  unsigned type;
  unsigned flags;
  unsigned data[12];
} ef_filter_spec;

typedef struct ef_filter_cookie {
  int filter_id;
  int filter_type;
} ef_filter_cookie;

/* Return zero for an unallocated VI. */
ef_vi_inline int ef_vi_receive_capacity(const ef_vi *vi)
{
  if (vi == NULL || vi->ep_state == NULL)
    return 0;
  return (int) (vi->vi_rxq.mask + 1u);
}

ef_vi_inline int ef_vi_receive_fill_level(const ef_vi *vi)
{
  if (vi == NULL || vi->ep_state == NULL)
    return 0;
  return (int) (vi->ep_state->rxq.added - vi->ep_state->rxq.removed);
}

ef_vi_inline int ef_vi_receive_space(const ef_vi *vi)
{
  if (vi == NULL || vi->ep_state == NULL)
    return 0;
  return ef_vi_receive_capacity(vi) - ef_vi_receive_fill_level(vi);
}

ef_vi_inline int ef_vi_receive_prefix_len(const ef_vi *vi)
{
  return vi != NULL ? (int) vi->rx_prefix_len : 0;
}

ef_vi_inline int ef_vi_receive_buffer_len(const ef_vi *vi)
{
  return vi != NULL ? (int) vi->rx_buffer_len : 0;
}

ef_vi_inline void ef_vi_receive_set_buffer_len(ef_vi *vi, unsigned bytes)
{
  if (vi != NULL)
    vi->rx_buffer_len = bytes;
}

/* DMA addresses are page-aligned tokens, not real hardware I/O addresses. */
ef_vi_inline ef_addr ef_memreg_dma_addr(const ef_memreg *mr, size_t offset)
{
  if (mr == NULL || mr->mr_dma_addrs == NULL)
    return (ef_addr) 0;
  return mr->mr_dma_addrs[offset >> EF_VI_NIC_PAGE_SHIFT] |
         (ef_addr) (offset & EF_VI_NIC_PAGE_MASK);
}

/* Commit 2: mock driver, protection domain, and memory registration. */
int ef_driver_open(ef_driver_handle *out);
int ef_driver_close(ef_driver_handle handle);
int ef_pd_alloc(ef_pd *pd, ef_driver_handle pd_dh, int ifindex,
                enum ef_pd_flags flags);
int ef_pd_free(ef_pd *pd, ef_driver_handle pd_dh);
int ef_memreg_alloc(ef_memreg *mr, ef_driver_handle mr_dh,
                    ef_pd *pd, ef_driver_handle pd_dh,
                    void *memory, size_t bytes);
int ef_memreg_free(ef_memreg *mr, ef_driver_handle mr_dh);

/* Commit 3: VI allocation, filters, and receive descriptor posting. */
int ef_vi_alloc_from_pd(ef_vi *vi, ef_driver_handle vi_dh,
                        ef_pd *pd, ef_driver_handle pd_dh,
                        int evq_capacity, int rxq_capacity, int txq_capacity,
                        ef_vi *evq_opt, ef_driver_handle evq_dh,
                        enum ef_vi_flags flags);
int ef_vi_free(ef_vi *vi, ef_driver_handle vi_dh);
void ef_filter_spec_init(ef_filter_spec *spec, enum ef_filter_flags flags);
int ef_filter_spec_set_ip4_local(ef_filter_spec *spec, int protocol,
                                 unsigned host_be32, int port_be16);
int ef_vi_filter_add(ef_vi *vi, ef_driver_handle vi_dh,
                     const ef_filter_spec *spec, ef_filter_cookie *cookie);
int ef_vi_receive_init(ef_vi *vi, ef_addr addr, ef_request_id request_id);
void ef_vi_receive_push(ef_vi *vi);
int ef_vi_receive_post(ef_vi *vi, ef_addr addr, ef_request_id request_id);

/* Commit 4: polling and timestamp capture. */
int ef_eventq_poll(ef_vi *vi, ef_event *events, int events_len);
int ef_vi_receive_get_timestamp(ef_vi *vi, const void *pkt,
                                ef_timespec *out);
int ef_vi_receive_get_timestamp_with_sync_flags(ef_vi *vi,
                                                const void *pkt,
                                                ef_timespec *out,
                                                unsigned *flags_out);
int ef_vi_receive_get_precise_timestamp(ef_vi *vi, const void *pkt,
                                        ef_precisetime *out);

#ifdef __cplusplus
}
#endif
#endif /* EFVI_MOCK_ETHERFABRIC_VI_H */
