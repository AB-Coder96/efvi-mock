/* SPDX-License-Identifier: BSD-2-Clause */
#define _GNU_SOURCE
#include "mock_internal.h"
#include "mock_packet.h"
#include <errno.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

struct packet_rx_metadata {
  bool has_vlan;
  uint16_t tpid;
  uint16_t tci;
};

/* One nonblocking packet-socket receive.  MSG_TRUNC reports the wire
 * length when the supplied buffer is shorter than the frame. */
static ssize_t receive_frame(int fd, uint8_t *dst, size_t cap,
                             struct packet_rx_metadata *meta)
{
  struct iovec iov = { .iov_base = dst, .iov_len = cap };
  struct sockaddr_ll sender = {0};
  char ancillary[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
  struct msghdr msg = {0};
  msg.msg_name = &sender;
  msg.msg_namelen = sizeof(sender);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ancillary;
  msg.msg_controllen = sizeof(ancillary);
  ssize_t n;
  do {
    n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return -errno;

  meta->has_vlan = false;
  meta->tpid = 0x8100u;
  meta->tci = 0;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_PACKET || cmsg->cmsg_type != PACKET_AUXDATA ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(struct tpacket_auxdata)))
      continue;
    const struct tpacket_auxdata *aux = (const void *)CMSG_DATA(cmsg);
    if (!(aux->tp_status & TP_STATUS_VLAN_VALID)) continue;
    meta->has_vlan = true;
    meta->tci = aux->tp_vlan_tci;
#ifdef TP_STATUS_VLAN_TPID_VALID
    if (aux->tp_status & TP_STATUS_VLAN_TPID_VALID)
      meta->tpid = aux->tp_vlan_tpid;
#endif
  }
  return n;
}

static void discard_event(ef_event *event, const struct ef_mock_rx_descriptor *d,
                          size_t frame_bytes)
{
  memset(event, 0, sizeof(*event));
  event->rx_discard.type = EF_EVENT_TYPE_RX_DISCARD;
  event->rx_discard.rq_id = (uint32_t)d->request_id;
  event->rx_discard.len = (uint16_t)(frame_bytes > UINT16_MAX ? UINT16_MAX : frame_bytes);
  event->rx_discard.subtype = EF_EVENT_RX_DISCARD_TRUNC;
}

static struct ef_mock_ipv4_udp_filter active_filter(
    const struct ef_mock_vi_context *ctx)
{
  struct ef_mock_ipv4_udp_filter filter;
  filter.address_be = ctx->filter_addr_be;
  filter.port_be = ctx->filter_port_be;
  return filter;
}

/* Called only when there are no pushed descriptors.  Filtering must still
 * run, otherwise unrelated traffic produces spurious no-descriptor events. */
static int receive_without_descriptor(struct ef_mock_vi_context *ctx,
                                      ef_event *event)
{
  uint8_t scratch[256];
  const struct ef_mock_ipv4_udp_filter filter = active_filter(ctx);
  for (unsigned inspected = 0; inspected < 128u; ++inspected) {
    struct packet_rx_metadata meta;
    ssize_t n = receive_frame(ctx->socket_fd, scratch, sizeof(scratch), &meta);
    if (n < 0) return n == -EAGAIN || n == -EWOULDBLOCK ? 0 : (int)n;
    size_t copied = (size_t)n < sizeof(scratch) ? (size_t)n : sizeof(scratch);
    if (meta.has_vlan && copied >= EFVI_MOCK_ETH_HEADER_BYTES &&
        copied + EFVI_MOCK_VLAN_HEADER_BYTES <= sizeof(scratch)) {
      int rc = ef_mock_packet_restore_vlan(scratch, &copied, sizeof(scratch),
                                           meta.tpid, meta.tci);
      if (rc < 0) continue;
    }
    if (!ef_mock_packet_matches_ip4_udp(scratch, copied, &filter, NULL))
      continue;
    memset(event, 0, sizeof(*event));
    event->rx_no_desc_trunc.type = EF_EVENT_TYPE_RX_NO_DESC_TRUNC;
    ++ctx->packets_dropped;
    return 1;
  }
  return 0;
}

int ef_eventq_poll(ef_vi *vi, ef_event *events, int events_len)
{
  if (!vi || !vi->mock_private || !vi->ep_state ||
      !events || events_len <= 0) return -EINVAL;
  struct ef_mock_vi_context *ctx = vi->mock_private;
  if (!ctx->filtered) return 0;
  ++ctx->poll_generation;
  const struct ef_mock_ipv4_udp_filter filter = active_filter(ctx);
  int completed = 0;
  size_t inspected = 0;
  size_t budget = (size_t)events_len * 64u + 64u;
  while (completed < events_len && inspected < budget) {
    if (vi->ep_state->rxq.removed == vi->ep_state->rxq.posted) {
      int rc = receive_without_descriptor(ctx, &events[completed]);
      if (rc <= 0) return completed ? completed : rc;
      ++completed;
      continue;
    }
    uint32_t slot = vi->ep_state->rxq.removed & vi->vi_rxq.mask;
    struct ef_mock_rx_descriptor *d = &ctx->descriptors[slot];
    struct packet_rx_metadata meta;
    ssize_t n = receive_frame(ctx->socket_fd, d->packet, d->buffer_bytes, &meta);
    if (n < 0) {
      if (n == -EAGAIN || n == -EWOULDBLOCK) break;
      return completed ? completed : (int)n;
    }
    ++inspected;
    size_t copied = (size_t)n < d->buffer_bytes ? (size_t)n : d->buffer_bytes;
    size_t frame_bytes = (size_t)n;
    bool truncated = (size_t)n > d->buffer_bytes;

    /* PACKET_AUXDATA reports VLAN tags that the driver stripped from the
     * skb.  Restore the 4-byte outer header in the application buffer so
     * that parsing the packet matches on-wire 802.1Q/802.1ad layout. */
    if (meta.has_vlan) {
      frame_bytes += EFVI_MOCK_VLAN_HEADER_BYTES;
      if (!truncated) {
        int rc = ef_mock_packet_restore_vlan(d->packet, &copied,
                                             d->buffer_bytes, meta.tpid,
                                             meta.tci);
        if (rc < 0) truncated = true;
      }
    }
    if (!ef_mock_packet_matches_ip4_udp(d->packet, copied, &filter, NULL)) {
      /* Packet is not for this VI: leave descriptor queued. */
      continue;
    }
    if (truncated || frame_bytes > EFVI_MOCK_MAX_FRAME_BYTES) {
      discard_event(&events[completed], d, frame_bytes);
      d->timestamp_valid = false;
      ++ctx->packets_dropped;
    }
    else {
      bool multicast = false;
      (void)ef_mock_packet_matches_ip4_udp(d->packet, copied,
                                            &filter, &multicast);
      memset(&events[completed], 0, sizeof(events[completed]));
      events[completed].rx.type = EF_EVENT_TYPE_RX;
      events[completed].rx.rq_id = (uint32_t)d->request_id;
      events[completed].rx.len = (uint16_t)frame_bytes;
      events[completed].rx.flags = EF_EVENT_FLAG_SOP |
          (multicast ? EF_EVENT_FLAG_MULTICAST : 0);
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
  if (!rc) *flags_out = 0;
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
