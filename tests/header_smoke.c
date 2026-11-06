/* SPDX-License-Identifier: BSD-2-Clause */
#include <etherfabric/vi.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define EFVI_TEST_STATIC_ASSERT static_assert
#else
#define EFVI_TEST_STATIC_ASSERT _Static_assert
#endif
EFVI_TEST_STATIC_ASSERT(sizeof(ef_addr) == 8, "ef_addr must be 64 bits");
EFVI_TEST_STATIC_ASSERT(sizeof(ef_request_id) == 4, "ef_request_id must be 32 bits");
EFVI_TEST_STATIC_ASSERT(EF_VI_NIC_PAGE_SIZE == 4096u, "NIC page size must be 4096");

int main(void)
{
  ef_vi vi;
  ef_vi_state state;
  ef_event event;
  ef_timespec stamp;
  ef_precisetime precise;
  ef_memreg mr;
  ef_addr pages[2];

  memset(&vi, 0, sizeof(vi));
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  memset(&stamp, 0, sizeof(stamp));
  memset(&precise, 0, sizeof(precise));
  memset(&mr, 0, sizeof(mr));

  assert(ef_vi_receive_capacity(&vi) == 0);
  assert(ef_vi_receive_space(&vi) == 0);
  assert(ef_vi_receive_fill_level(&vi) == 0);

  vi.ep_state = &state;
  vi.vi_rxq.mask = 7;
  state.rxq.added = 5;
  state.rxq.removed = 2;
  assert(ef_vi_receive_capacity(&vi) == 8);
  assert(ef_vi_receive_fill_level(&vi) == 3);
  assert(ef_vi_receive_space(&vi) == 5);

  ef_vi_receive_set_buffer_len(&vi, 2048);
  assert(ef_vi_receive_buffer_len(&vi) == 2048);
  assert(ef_vi_receive_prefix_len(&vi) == 0);

  event.rx.type = EF_EVENT_TYPE_RX;
  event.rx.rq_id = 17;
  event.rx.len = 128;
  event.rx.flags = EF_EVENT_FLAG_SOP;
  assert(EF_EVENT_TYPE(event) == EF_EVENT_TYPE_RX);
  assert(EF_EVENT_RX_RQ_ID(event) == 17);
  assert(EF_EVENT_RX_BYTES(event) == 128);
  assert(EF_EVENT_RX_SOP(event));

  pages[0] = UINT64_C(0x10000000);
  pages[1] = UINT64_C(0x20000000);
  mr.mr_dma_addrs = pages;
  assert(ef_memreg_dma_addr(&mr, 4096 + 64) == pages[1] + 64);

  (void)stamp;
  (void)precise;
  return 0;
}
