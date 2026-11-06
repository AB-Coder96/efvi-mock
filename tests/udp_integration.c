/* SPDX-License-Identifier: BSD-2-Clause */
#define _POSIX_C_SOURCE 200809L
#include <etherfabric/vi.h>
#include <arpa/inet.h>
#undef NDEBUG /* Assertions in tests must run even in Release builds. */
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TEST_BUFFERS 8u
#define BUFFER_BYTES 2048u

static uint16_t checksum(const uint8_t *p, size_t len)
{
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
  while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
  return (uint16_t)~sum;
}

static int wait_for_event(ef_vi *vi, ef_event *event)
{
  for (int i = 0; i < 5000; ++i) {
    int n = ef_eventq_poll(vi, event, 1);
    if (n != 0) return n;
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    (void)nanosleep(&pause, NULL);
  }
  return 0;
}

int main(void)
{
  ef_driver_handle dh;
  ef_pd pd = {0};
  ef_memreg mr = {0};
  ef_vi vi = {0};
  ef_event ev;
  ef_timespec ts;
  ef_precisetime precise;
  unsigned flags = UINT32_MAX;
  void *mem = NULL;
  int tx = -1;

  assert(ef_driver_open(&dh) == 0);
  assert(ef_pd_alloc(&pd, dh, 0, EF_PD_DEFAULT) == 0);
  assert(ef_driver_close(dh) == -EBUSY);
  assert(posix_memalign(&mem, EF_VI_NIC_PAGE_SIZE,
                        TEST_BUFFERS * BUFFER_BYTES) == 0);
  memset(mem, 0, TEST_BUFFERS * BUFFER_BYTES);
  assert(ef_memreg_alloc(&mr, dh, &pd, dh, mem,
                         TEST_BUFFERS * BUFFER_BYTES) == 0);
  assert(ef_pd_free(&pd, dh) == -EBUSY);
  assert(ef_memreg_dma_addr(&mr, 4096) != ef_memreg_dma_addr(&mr, 0));
  int alloc_rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, TEST_BUFFERS,
                                     0, NULL, -1, EF_VI_RX_TIMESTAMPS);
  if (alloc_rc == -EPERM || alloc_rc == -EACCES) {
    /* AF_PACKET/SOCK_RAW requires CAP_NET_RAW in the test environment. */
    assert(ef_memreg_free(&mr, dh) == 0);
    assert(ef_pd_free(&pd, dh) == 0);
    assert(ef_driver_close(dh) == 0);
    free(mem);
    puts("raw_integration: SKIP (CAP_NET_RAW required)");
    return 77;
  }
  assert(alloc_rc == 0);
  assert(ef_vi_receive_capacity(&vi) == TEST_BUFFERS);
  assert(ef_vi_receive_space(&vi) == TEST_BUFFERS);
  assert(ef_vi_receive_init(&vi, ef_memreg_dma_addr(&mr, 0), 11) == 0);
  assert(ef_memreg_free(&mr, dh) == -EBUSY);
  assert(ef_vi_receive_fill_level(&vi) == 1);
  ef_vi_receive_push(&vi);

  /* Choose an unused port on loopback, then bind the emulator to it. */
  tx = socket(AF_INET, SOCK_DGRAM, 0);
  assert(tx >= 0);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  assert(probe >= 0);
  assert(bind(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  socklen_t namelen = sizeof(addr);
  assert(getsockname(probe, (struct sockaddr *)&addr, &namelen) == 0);
  close(probe);

  ef_filter_spec filter;
  ef_filter_cookie cookie;
  ef_filter_spec_init(&filter, EF_FILTER_FLAG_NONE);
  assert(ef_filter_spec_set_ip4_local(&filter, IPPROTO_UDP,
                                     addr.sin_addr.s_addr, addr.sin_port) == 0);
  assert(ef_vi_filter_add(&vi, dh, &filter, &cookie) == 0);
  assert(ef_vi_filter_add(&vi, dh, &filter, NULL) == -EALREADY);
  assert(cookie.filter_id == 1);

  const char message[] = "raw socket integration datagram";
  assert(sendto(tx, message, sizeof(message), 0, (struct sockaddr *)&addr,
                sizeof(addr)) == (ssize_t)sizeof(message));
  assert(wait_for_event(&vi, &ev) == 1);
  assert(EF_EVENT_TYPE(ev) == EF_EVENT_TYPE_RX);
  assert(EF_EVENT_RX_RQ_ID(ev) == 11);
  assert(EF_EVENT_RX_BYTES(ev) == 42 + sizeof(message));
  assert(EF_EVENT_RX_SOP(ev));
  const uint8_t *pkt = mem;
  assert(pkt[12] == 0x08 && pkt[13] == 0x00);
  assert(pkt[14] == 0x45 && pkt[23] == IPPROTO_UDP);
  assert(pkt[16] == 0 && pkt[17] == 20 + 8 + sizeof(message));
  assert(checksum(pkt + 14, 20) == 0);
  assert(memcmp(pkt + 30, &addr.sin_addr.s_addr, 4) == 0);
  assert(memcmp(pkt + 36, &addr.sin_port, 2) == 0);
  assert(memcmp(pkt + 42, message, sizeof(message)) == 0);
  assert(ef_vi_receive_get_timestamp(&vi, pkt, &ts) == 0);
  assert(ts.tv_sec > 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L);
  assert(ef_vi_receive_get_timestamp_with_sync_flags(&vi, pkt, &ts, &flags) == 0);
  assert(flags == 0);
  assert(ef_vi_receive_get_precise_timestamp(&vi, pkt, &precise) == 0);
  assert(precise.tv_nsec == (uint32_t)ts.tv_nsec);
  assert(precise.tv_nsec_frac == 0 && precise.tv_flags == 0);
  assert(ef_vi_receive_fill_level(&vi) == 0);
  assert(ef_eventq_poll(&vi, &ev, 1) == 0);
  assert(ef_vi_receive_get_timestamp(&vi, pkt, &ts) == -ENOENT);

  /* No descriptors: the application must observe an explicit drop event. */
  assert(sendto(tx, message, sizeof(message), 0, (struct sockaddr *)&addr,
                sizeof(addr)) == (ssize_t)sizeof(message));
  assert(wait_for_event(&vi, &ev) == 1);
  assert(EF_EVENT_TYPE(ev) == EF_EVENT_TYPE_RX_NO_DESC_TRUNC);

  /* User buffer smaller than the datagram: RX_DISCARD, not memory overrun. */
  ef_vi_receive_set_buffer_len(&vi, 64);
  assert(ef_vi_receive_post(&vi, ef_memreg_dma_addr(&mr, BUFFER_BYTES), 12) == 0);
  assert(sendto(tx, message, sizeof(message), 0, (struct sockaddr *)&addr,
                sizeof(addr)) == (ssize_t)sizeof(message));
  assert(wait_for_event(&vi, &ev) == 1);
  assert(EF_EVENT_TYPE(ev) == EF_EVENT_TYPE_RX_DISCARD);
  assert(EF_EVENT_RX_DISCARD_TYPE(ev) == EF_EVENT_RX_DISCARD_TRUNC);
  assert(EF_EVENT_RX_DISCARD_RQ_ID(ev) == 12);

  ef_vi_receive_set_buffer_len(&vi, BUFFER_BYTES);
  for (unsigned i = 0; i < TEST_BUFFERS; ++i)
    assert(ef_vi_receive_init(&vi, ef_memreg_dma_addr(&mr, i * BUFFER_BYTES),
                              100 + (int)i) == 0);
  assert(ef_vi_receive_space(&vi) == 0);
  assert(ef_vi_receive_init(&vi, ef_memreg_dma_addr(&mr, 0), 999) == -EAGAIN);
  ef_vi_receive_push(&vi);
  for (unsigned i = 0; i < TEST_BUFFERS; ++i)
    assert(sendto(tx, message, sizeof(message), 0,
                  (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)sizeof(message));
  for (unsigned i = 0; i < TEST_BUFFERS; ++i) {
    assert(wait_for_event(&vi, &ev) == 1);
    assert(EF_EVENT_TYPE(ev) == EF_EVENT_TYPE_RX);
    assert(EF_EVENT_RX_RQ_ID(ev) == 100 + i);
  }
  assert(ef_vi_receive_fill_level(&vi) == 0);
  assert(ef_memreg_free(&mr, dh) == 0);
  assert(ef_pd_free(&pd, dh) == -EBUSY); /* VI still holds the PD. */
  assert(ef_vi_free(&vi, dh) == 0);
  assert(ef_pd_free(&pd, dh) == 0);
  assert(ef_driver_close(dh) == 0);
  close(tx);
  free(mem);
  puts("raw_integration: PASS");
  return 0;
}
