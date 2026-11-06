/* SPDX-License-Identifier: BSD-2-Clause */
#include "mock_packet.h"
#include <arpa/inet.h>
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void be16(uint8_t *p, unsigned n)
{
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)n;
}

static size_t build_udp_frame(uint8_t *f)
{
  /* Ethernet II + IPv4 + UDP, destination 239.42.0.1:9000. */
  memset(f, 0, 256);
  f[0] = 1; f[1] = 0; f[2] = 0x5e; f[3] = 42; f[5] = 1;
  f[12] = 0x08; f[13] = 0;
  f[14] = 0x45;
  be16(f + 16, 28);
  f[22] = 64;
  f[23] = 17;
  f[26] = 10; f[29] = 2;
  f[30] = 239; f[31] = 42; f[32] = 0; f[33] = 1;
  be16(f + 34, 1111);
  be16(f + 36, 9000);
  be16(f + 38, 8);
  return 42;
}

int main(void)
{
  uint8_t frame[256];
  size_t len = build_udp_frame(frame);
  struct ef_mock_ipv4_udp_filter f = {0};
  assert(inet_pton(AF_INET, "239.42.0.1", &f.address_be) == 1);
  f.port_be = htons(9000);
  bool multicast = false;
  assert(ef_mock_packet_matches_ip4_udp(frame, len, &f, &multicast));
  assert(multicast);
  f.port_be = htons(9001);
  assert(!ef_mock_packet_matches_ip4_udp(frame, len, &f, NULL));
  f.port_be = htons(9000);

  /* Reconstruct a stripped 802.1Q tag; retain both PCP and VID. */
  assert(ef_mock_packet_restore_vlan(frame, &len, sizeof(frame),
                                     0x8100, 0xa123) == 0);
  assert(len == 46);
  assert(frame[12] == 0x81 && frame[13] == 0x00);
  assert(frame[14] == 0xa1 && frame[15] == 0x23);
  assert(frame[16] == 0x08 && frame[17] == 0x00);
  assert(ef_mock_packet_matches_ip4_udp(frame, len, &f, &multicast));
  assert(multicast);

  /* Add an 802.1ad service tag, constructing a QinQ packet. */
  assert(ef_mock_packet_restore_vlan(frame, &len, sizeof(frame),
                                     0x88a8, 0x0064) == 0);
  assert(len == 50);
  assert(frame[12] == 0x88 && frame[13] == 0xa8);
  assert(frame[16] == 0x81 && frame[17] == 0x00);
  assert(ef_mock_packet_matches_ip4_udp(frame, len, &f, NULL));

  /* Invalid/malformed headers may not reach a VI. */
  assert(!ef_mock_packet_matches_ip4_udp(frame, 18, &f, NULL));
  assert(!ef_mock_packet_matches_ip4_udp(frame, 49, &f, NULL));
  uint8_t corrupt[256];
  memcpy(corrupt, frame, len);
  corrupt[20] = 0;
  assert(!ef_mock_packet_matches_ip4_udp(corrupt, len, &f, NULL));
  assert(ef_mock_packet_restore_vlan(frame, &len, len, 0x8100, 1) == -ENOBUFS);
  assert(ef_mock_packet_restore_vlan(frame, &len, sizeof(frame), 0x0800, 1)
         == -EPROTO);
  puts("packet_unit: PASS");
  return 0;
}
