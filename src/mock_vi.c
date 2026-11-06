/* SPDX-License-Identifier: BSD-2-Clause */
#define _GNU_SOURCE
#include "mock_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool is_power_of_two(uint32_t n) { return n && !(n & (n - 1u)); }

int ef_vi_alloc_from_pd(ef_vi *vi, ef_driver_handle vi_dh,
                        ef_pd *pd, ef_driver_handle pd_dh,
                        int evq_capacity, int rxq_capacity, int txq_capacity,
                        ef_vi *evq_opt, ef_driver_handle evq_dh,
                        enum ef_vi_flags flags)
{
  (void)evq_dh;
  if (!vi || vi->mock_private || !pd || !pd->mock_private) return -EINVAL;
  if (evq_opt || txq_capacity > 0 || (flags & ~EF_VI_RX_TIMESTAMPS))
    return -EOPNOTSUPP;
  if (txq_capacity < -1 || evq_capacity < -1 || evq_capacity == 0 ||
      rxq_capacity < -1 || rxq_capacity == 0) return -EINVAL;
  uint32_t cap = rxq_capacity == -1 ? EFVI_MOCK_DEFAULT_RX_CAPACITY
                                    : (uint32_t)rxq_capacity;
  if (!is_power_of_two(cap) || cap < 2 || cap > EFVI_MOCK_MAX_RX_CAPACITY)
    return -EINVAL;
  if (evq_capacity > 0 && (uint32_t)evq_capacity < cap)
    return -EINVAL;
  if (vi_dh != pd_dh) return -EBADF;
  int rc = ef_mock_pd_acquire(pd, vi_dh);
  if (rc < 0) return rc;

  struct ef_mock_vi_context *ctx = calloc(1, sizeof(*ctx));
  ef_vi_state *state = calloc(1, sizeof(*state));
  struct ef_mock_rx_descriptor *descs = calloc(cap, sizeof(*descs));
  uint32_t *ids = calloc(cap, sizeof(*ids));
  if (!ctx || !state || !descs || !ids) {
    rc = -ENOMEM;
    goto fail;
  }
  ctx->socket_fd = -1;
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) { rc = -errno; goto fail; }
  ctx->socket_fd = fd;
  int enable = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ||
      setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable))) {
    rc = -errno;
    goto fail;
  }
#ifdef IP_MULTICAST_ALL
  int disable = 0;
  (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &disable, sizeof(disable));
#endif
  ctx->pd = pd;
  ctx->driver = vi_dh;
  ctx->ifindex = ((const struct ef_mock_pd_state *)pd->mock_private)->ifindex;
  ctx->rx_capacity = cap;
  ctx->descriptors = descs;
  vi->vi_rxq.mask = cap - 1;
  vi->vi_rxq.descriptors = descs;
  vi->vi_rxq.ids = ids;
  vi->ep_state = state;
  vi->vi_flags = flags;
  vi->rx_buffer_len = EFVI_MOCK_DEFAULT_BUFFER_BYTES;
  vi->rx_prefix_len = 0;
  vi->mock_private = ctx;
  return 0;

fail:
  if (ctx && ctx->socket_fd >= 0) close(ctx->socket_fd);
  free(ids);
  free(descs);
  free(state);
  free(ctx);
  ef_mock_pd_release(pd);
  return rc;
}

int ef_vi_free(ef_vi *vi, ef_driver_handle dh)
{
  if (!vi || !vi->mock_private || !vi->ep_state) return -EINVAL;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  if (ctx->driver != dh) return -EBADF;
  for (uint32_t i = vi->ep_state->rxq.removed;
       i != vi->ep_state->rxq.added; ++i) {
    struct ef_mock_rx_descriptor *d = &ctx->descriptors[i & vi->vi_rxq.mask];
    if (d->registration) ef_mock_release_registration(d->registration);
  }
  close(ctx->socket_fd);
  ef_mock_pd_release(ctx->pd);
  free(ctx->descriptors);
  free(vi->vi_rxq.ids);
  free(vi->ep_state);
  free(ctx);
  memset(vi, 0, sizeof(*vi));
  return 0;
}

void ef_filter_spec_init(ef_filter_spec *spec, enum ef_filter_flags flags)
{
  if (!spec) return;
  memset(spec, 0, sizeof(*spec));
  spec->flags = (unsigned)flags;
}

int ef_filter_spec_set_ip4_local(ef_filter_spec *spec, int protocol,
                                 unsigned host_be32, int port_be16)
{
  if (!spec || protocol != IPPROTO_UDP || port_be16 <= 0 ||
      (unsigned)port_be16 > UINT16_MAX) return -EINVAL;
  if (spec->flags != EF_FILTER_FLAG_NONE) return -EOPNOTSUPP;
  spec->type = EFVI_MOCK_FILTER_IP4_LOCAL;
  spec->data[0] = (unsigned)protocol;
  spec->data[1] = host_be32; /* Network order, like sockaddr_in.sin_addr.s_addr. */
  spec->data[2] = (unsigned)port_be16; /* Network order, like sin_port. */
  return 0;
}

int ef_vi_filter_add(ef_vi *vi, ef_driver_handle dh,
                     const ef_filter_spec *spec, ef_filter_cookie *cookie)
{
  if (!vi || !vi->mock_private || !spec) return -EINVAL;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  if (ctx->driver != dh) return -EBADF;
  if (ctx->filtered) return -EALREADY;
  if (spec->type != EFVI_MOCK_FILTER_IP4_LOCAL ||
      spec->data[0] != IPPROTO_UDP || spec->flags != 0 ||
      !spec->data[2] || spec->data[2] > UINT16_MAX) return -EOPNOTSUPP;

  uint32_t ip_be = spec->data[1];
  uint16_t port_be = (uint16_t)spec->data[2];
  bool is_mcast = IN_MULTICAST(ntohl(ip_be));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = port_be;
  addr.sin_addr.s_addr = is_mcast ? htonl(INADDR_ANY) : ip_be;
  if (bind(ctx->socket_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
    return -errno;
  if (is_mcast) {
    struct ip_mreqn membership;
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = ip_be;
    membership.imr_ifindex = (int)ctx->ifindex;
    if (setsockopt(ctx->socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &membership, sizeof(membership)) < 0)
      return -errno;
  }
  ctx->filter_addr_be = ip_be;
  ctx->filter_port_be = port_be;
  ctx->filtered = true;
  if (cookie) {
    cookie->filter_id = 1;
    cookie->filter_type = (int)spec->type;
  }
  return 0;
}

int ef_vi_receive_init(ef_vi *vi, ef_addr addr, ef_request_id request_id)
{
  if (!vi || !vi->mock_private || !vi->ep_state) return -EINVAL;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  if (request_id < 0 || (addr & (EF_VI_DMA_ALIGN - 1u))) return -EINVAL;
  if (vi->rx_buffer_len < EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES ||
      vi->rx_buffer_len > EFVI_MOCK_MAX_FRAME_BYTES) return -EINVAL;
  if (ef_vi_receive_space(vi) <= 0) return -EAGAIN;
  struct ef_mock_memreg_state *reg;
  uint8_t *ptr;
  int rc = ef_mock_lookup_and_hold(ctx->pd, addr, vi->rx_buffer_len,
                                   &reg, &ptr);
  if (rc < 0) return rc;
  uint32_t n = vi->ep_state->rxq.added++ & vi->vi_rxq.mask;
  struct ef_mock_rx_descriptor *d = &ctx->descriptors[n];
  d->address = addr;
  d->request_id = request_id;
  d->packet = ptr;
  d->buffer_bytes = vi->rx_buffer_len;
  d->registration = reg;
  d->timestamp_valid = false;
  d->poll_generation = 0;
  vi->vi_rxq.ids[n] = (uint32_t)request_id;
  return 0;
}

void ef_vi_receive_push(ef_vi *vi)
{
  if (vi && vi->mock_private && vi->ep_state)
    vi->ep_state->rxq.posted = vi->ep_state->rxq.added;
}

int ef_vi_receive_post(ef_vi *vi, ef_addr addr, ef_request_id request_id)
{
  int rc = ef_vi_receive_init(vi, addr, request_id);
  if (!rc) ef_vi_receive_push(vi);
  return rc;
}
