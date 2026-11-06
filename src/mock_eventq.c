/* SPDX-License-Identifier: BSD-2-Clause */
#define _GNU_SOURCE
#include "mock_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

static void net16(uint8_t *p, uint16_t n)
{
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)n;
}

static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 2)
    sum += (uint32_t)((uint16_t)p[i] << 8 | p[i + 1]);
  while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
  return (uint16_t)~sum;
}

static void make_frame_headers(struct ef_mock_vi_context *ctx,
                               uint8_t *buf, size_t payload_bytes,
                               const struct sockaddr_in *remote,
                               uint32_t dest_ip_be)
{
  memset(buf, 0, EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES);
  if (IN_MULTICAST(ntohl(dest_ip_be))) {
    uint32_t multicast_ip = ntohl(dest_ip_be);
    buf[0] = 0x01; buf[1] = 0x00; buf[2] = 0x5e;
    buf[3] = (uint8_t)((multicast_ip >> 16) & 0x7fu);
    buf[4] = (uint8_t)(multicast_ip >> 8);
    buf[5] = (uint8_t)multicast_ip;
  }
  else {
    buf[0] = 0x02; buf[5] = 0x01;
  }
  buf[6] = 0x02; buf[11] = 0x02; /* Deterministic synthetic source MAC. */
  buf[12] = 0x08; buf[13] = 0x00; /* Ethernet IPv4 ethertype. */

  uint8_t *ip = buf + 14;
  ip[0] = 0x45;           /* IPv4 without options. */
  net16(ip + 2, (uint16_t)(20u + 8u + payload_bytes));
  net16(ip + 4, ctx->next_ip_id++);
  net16(ip + 6, 0x4000); /* Synthetic unfragmented datagram. */
  ip[8] = 64;
  ip[9] = IPPROTO_UDP;
  memcpy(ip + 12, &remote->sin_addr.s_addr, 4);
  memcpy(ip + 16, &dest_ip_be, 4);
  net16(ip + 10, ip_checksum(ip, 20));

  uint8_t *udp = buf + 34;
  memcpy(udp, &remote->sin_port, 2);
  memcpy(udp + 2, &ctx->filter_port_be, 2);
  net16(udp + 4, (uint16_t)(8u + payload_bytes));
  /* An IPv4 UDP checksum of zero is legal; it is not a hardware checksum. */
}

static void discard_event(ef_event *event, struct ef_mock_rx_descriptor *d,
                          size_t frame_bytes)
{
  memset(event, 0, sizeof(*event));
  event->rx_discard.type = EF_EVENT_TYPE_RX_DISCARD;
  event->rx_discard.rq_id = (uint32_t)d->request_id;
  event->rx_discard.len = (uint16_t)(frame_bytes > UINT16_MAX ? UINT16_MAX : frame_bytes);
  event->rx_discard.subtype = EF_EVENT_RX_DISCARD_TRUNC;
}

/* Drain at most one datagram when no pushed descriptor exists. */
static int discard_without_descriptor(struct ef_mock_vi_context *ctx,
                                      ef_event *event)
{
  char scratch;
  ssize_t n;
  do {
    n = recv(ctx->socket_fd, &scratch, sizeof(scratch), MSG_DONTWAIT | MSG_TRUNC);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
  memset(event, 0, sizeof(*event));
  event->rx_no_desc_trunc.type = EF_EVENT_TYPE_RX_NO_DESC_TRUNC;
  ++ctx->packets_dropped;
  return 1;
}

int ef_eventq_poll(ef_vi *vi, ef_event *events, int events_len)
{
  if (!vi || !vi->mock_private || !vi->ep_state ||
      !events || events_len <= 0) return -EINVAL;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  if (!ctx->filtered) return 0;
  ++ctx->poll_generation;
  int completed = 0;
  while (completed < events_len) {
    if (vi->ep_state->rxq.removed == vi->ep_state->rxq.posted) {
      int rc = discard_without_descriptor(ctx, &events[completed]);
      if (rc <= 0) return completed ? completed : rc;
      ++completed;
      continue;
    }
    uint32_t slot = vi->ep_state->rxq.removed & vi->vi_rxq.mask;
    struct ef_mock_rx_descriptor *d = &ctx->descriptors[slot];
    struct sockaddr_in remote;
    struct iovec iov;
    struct msghdr msg;
    char ancillary[CMSG_SPACE(sizeof(struct in_pktinfo))];
    memset(&remote, 0, sizeof(remote));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = d->packet + EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES;
    iov.iov_len = d->buffer_bytes - EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES;
    msg.msg_name = &remote;
    msg.msg_namelen = sizeof(remote);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);
    ssize_t read_bytes;
    do {
      read_bytes = recvmsg(ctx->socket_fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
    } while (read_bytes < 0 && errno == EINTR);
    if (read_bytes < 0) {
      int saved = errno;
      if (saved == EAGAIN || saved == EWOULDBLOCK) break;
      return completed ? completed : -saved;
    }
    uint32_t dest_ip_be = ctx->filter_addr_be;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO &&
          cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
        const struct in_pktinfo *info = (const void *)CMSG_DATA(cmsg);
        dest_ip_be = info->ipi_addr.s_addr;
      }
    }
    /* A multicast socket bound to INADDR_ANY must still reject other groups. */
    if (ctx->filter_addr_be != htonl(INADDR_ANY) &&
        dest_ip_be != ctx->filter_addr_be) {
      ++ctx->packets_dropped;
      /* Filtered in software: do not consume an RX descriptor. */
      continue;
    }
    size_t length = EFVI_MOCK_ETH_IPV4_UDP_HEADER_BYTES +
                    (size_t)read_bytes;
    bool truncated = (msg.msg_flags & MSG_TRUNC) ||
                     (size_t)read_bytes > iov.iov_len ||
                     length > EFVI_MOCK_MAX_FRAME_BYTES;
    if (truncated) {
      discard_event(&events[completed], d, length);
      d->timestamp_valid = false;
      ++ctx->packets_dropped;
    }
    else {
      make_frame_headers(ctx, d->packet, (size_t)read_bytes,
                         &remote, dest_ip_be);
      memset(&events[completed], 0, sizeof(events[completed]));
      events[completed].rx.type = EF_EVENT_TYPE_RX;
      events[completed].rx.rq_id = (uint32_t)d->request_id;
      events[completed].rx.len = (uint16_t)length;
      events[completed].rx.flags = EF_EVENT_FLAG_SOP |
          (IN_MULTICAST(ntohl(dest_ip_be)) ? EF_EVENT_FLAG_MULTICAST : 0);
      if (vi->vi_flags & EF_VI_RX_TIMESTAMPS) {
        if (clock_gettime(CLOCK_REALTIME, &d->utc) == 0 &&
            clock_gettime(CLOCK_MONOTONIC, &d->monotonic) == 0) {
          d->timestamp_valid = true;
          d->poll_generation = ctx->poll_generation;
        }
        else {
          d->timestamp_valid = false;
        }
      }
      else {
        d->timestamp_valid = false;
      }
      ++ctx->packets_received;
    }
    ++vi->ep_state->rxq.removed;
    ef_mock_release_registration(d->registration);
    d->registration = NULL;
    ++completed;
  }
  return completed;
}

static int lookup_timestamp(ef_vi *vi, const void *pkt, struct timespec *ts)
{
  if (!vi || !vi->mock_private || !pkt || !ts) return -EINVAL;
  if (!(vi->vi_flags & EF_VI_RX_TIMESTAMPS)) return -ENODATA;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  for (uint32_t i = 0; i < ctx->rx_capacity; ++i) {
    const struct ef_mock_rx_descriptor *d = &ctx->descriptors[i];
    if (d->packet == pkt && d->timestamp_valid &&
        d->poll_generation == ctx->poll_generation) {
      *ts = d->utc;
      return 0;
    }
  }
  return -ENOENT;
}

int ef_vi_receive_get_timestamp(ef_vi *vi, const void *pkt, ef_timespec *out)
{
  return lookup_timestamp(vi, pkt, out);
}

int ef_vi_receive_get_timestamp_with_sync_flags(ef_vi *vi, const void *pkt,
                                                ef_timespec *out,
                                                unsigned *flags_out)
{
  if (!flags_out) return -EINVAL;
  int rc = lookup_timestamp(vi, pkt, out);
  if (!rc) *flags_out = 0; /* Software clock: no asserted PTP sync state. */
  return rc;
}

int ef_vi_receive_get_precise_timestamp(ef_vi *vi, const void *pkt,
                                        ef_precisetime *out)
{
  if (!out) return -EINVAL;
  struct timespec ts;
  int rc = lookup_timestamp(vi, pkt, &ts);
  if (rc < 0) return rc;
  out->tv_sec = ts.tv_sec;
  out->tv_nsec = (uint32_t)ts.tv_nsec;
  out->tv_nsec_frac = 0;
  out->tv_flags = 0;
  return 0;
}
