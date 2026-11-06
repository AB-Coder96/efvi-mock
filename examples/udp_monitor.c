/* SPDX-License-Identifier: BSD-2-Clause */
#define _POSIX_C_SOURCE 200809L
#include <etherfabric/vi.h>
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RX_SLOTS 128u
#define SLOT_BYTES 2048u

static volatile sig_atomic_t stop_requested;
static void on_signal(int signum) { (void)signum; stop_requested = 1; }

static unsigned read_be16(const uint8_t *p)
{
  return (unsigned)p[0] * 256u + p[1];
}

static void print_frame(const uint8_t *frame, unsigned bytes,
                        unsigned id, const ef_timespec *stamp)
{
  if (bytes < 14) return;
  unsigned type = read_be16(frame + 12);
  unsigned offset = 14;
  unsigned vlan = 0;
  unsigned priority = 0;
  unsigned tags = 0;
  while ((type == 0x8100 || type == 0x88a8 || type == 0x9100) &&
         offset + 4 <= bytes && tags < 4) {
    unsigned tci = read_be16(frame + offset);
    vlan = tci & 0x0fffu;
    priority = (tci >> 13) & 7u;
    type = read_be16(frame + offset + 2);
    offset += 4;
    ++tags;
  }
  unsigned payload_bytes = 0;
  if (type == 0x0800 && offset + 28 <= bytes) {
    const uint8_t *ip = frame + offset;
    unsigned ihl = (ip[0] & 15u) * 4u;
    if ((ip[0] >> 4) == 4 && ihl >= 20 && ip[9] == IPPROTO_UDP &&
        offset + ihl + 8 <= bytes)
      payload_bytes = bytes - offset - ihl - 8;
  }
  printf("%lld.%09ld id=%u frame_bytes=%u udp_payload_bytes=%u",
         (long long)stamp->tv_sec, stamp->tv_nsec, id, bytes, payload_bytes);
  if (tags) printf(" vlan_id=%u pcp=%u tags=%u", vlan, priority, tags);
  putchar('\n');
}

static int run(const char *ip, const char *port_string, const char *interface)
{
  char *end;
  errno = 0;
  unsigned long port = strtoul(port_string, &end, 10);
  if (errno || *end || port == 0 || port > UINT16_MAX) return -EINVAL;
  struct in_addr addr;
  if (inet_pton(AF_INET, ip, &addr) != 1) return -EINVAL;
  unsigned ifindex = 0;
  if (interface) {
    ifindex = if_nametoindex(interface);
    if (!ifindex) return -ENODEV;
  }
  ef_driver_handle dh = -1;
  ef_pd pd = {0};
  ef_memreg mr = {0};
  ef_vi vi = {0};
  void *buf = NULL;
  int rc = ef_driver_open(&dh);
  if (rc < 0) goto cleanup;
  rc = ef_pd_alloc(&pd, dh, (int)ifindex, EF_PD_DEFAULT);
  if (rc < 0) goto cleanup;
  rc = posix_memalign(&buf, EF_VI_NIC_PAGE_SIZE, RX_SLOTS * SLOT_BYTES);
  if (rc) { rc = -rc; goto cleanup; }
  rc = ef_memreg_alloc(&mr, dh, &pd, dh, buf, RX_SLOTS * SLOT_BYTES);
  if (rc < 0) goto cleanup;
  rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, RX_SLOTS, 0, NULL,
                           -1, EF_VI_RX_TIMESTAMPS);
  if (rc < 0) goto cleanup;
  ef_filter_spec filter;
  ef_filter_spec_init(&filter, EF_FILTER_FLAG_NONE);
  rc = ef_filter_spec_set_ip4_local(&filter, IPPROTO_UDP,
                                    addr.s_addr, htons((uint16_t)port));
  if (rc < 0) goto cleanup;
  rc = ef_vi_filter_add(&vi, dh, &filter, NULL);
  if (rc < 0) goto cleanup;
  for (unsigned i = 0; i < RX_SLOTS; ++i) {
    rc = ef_vi_receive_init(&vi, ef_memreg_dma_addr(&mr, i * SLOT_BYTES),
                             (ef_request_id)i);
    if (rc < 0) goto cleanup;
  }
  ef_vi_receive_push(&vi);
  printf("efvi-mock raw Ethernet RX %s:%lu interface=%s; Ctrl-C to stop\n",
         ip, port, interface ? interface : "any");
  fflush(stdout);
  while (!stop_requested) {
    ef_event events[32];
    int count = ef_eventq_poll(&vi, events, 32);
    if (count < 0) { rc = count; break; }
    if (!count) {
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&pause, NULL);
      continue;
    }
    for (int i = 0; i < count; ++i) {
      ef_event *ev = &events[i];
      if (EF_EVENT_TYPE(*ev) == EF_EVENT_TYPE_RX ||
          EF_EVENT_TYPE(*ev) == EF_EVENT_TYPE_RX_DISCARD) {
        unsigned id = EF_EVENT_TYPE(*ev) == EF_EVENT_TYPE_RX
                          ? EF_EVENT_RX_RQ_ID(*ev)
                          : EF_EVENT_RX_DISCARD_RQ_ID(*ev);
        uint8_t *pkt = (uint8_t *)buf + id * SLOT_BYTES;
        if (EF_EVENT_TYPE(*ev) == EF_EVENT_TYPE_RX) {
          ef_timespec stamp;
          if (ef_vi_receive_get_timestamp(&vi, pkt, &stamp) == 0)
            print_frame(pkt, EF_EVENT_RX_BYTES(*ev), id, &stamp);
        }
        else fprintf(stderr, "RX truncated id=%u\n", id);
        rc = ef_vi_receive_post(&vi, ef_memreg_dma_addr(&mr, id * SLOT_BYTES),
                                 (ef_request_id)id);
        if (rc < 0) goto cleanup;
      }
      else if (EF_EVENT_TYPE(*ev) == EF_EVENT_TYPE_RX_NO_DESC_TRUNC)
        fprintf(stderr, "RX dropped: no descriptor\n");
    }
    fflush(stdout);
  }
  if (rc >= 0) rc = 0;
cleanup:
  if (vi.mock_private) (void)ef_vi_free(&vi, dh);
  if (mr.mock_private) (void)ef_memreg_free(&mr, dh);
  if (pd.mock_private) (void)ef_pd_free(&pd, dh);
  if (dh >= 0) (void)ef_driver_close(dh);
  free(buf);
  return rc;
}

int main(int argc, char **argv)
{
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: %s <local-ip-or-multicast-group> <port> [interface]\n",
            argv[0]);
    return 2;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  int rc = run(argv[1], argv[2], argc == 4 ? argv[3] : NULL);
  if (rc < 0) {
    fprintf(stderr, "efvi-mock: %s\n", strerror(-rc));
    return 1;
  }
  return 0;
}
