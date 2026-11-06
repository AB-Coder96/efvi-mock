/* SPDX-License-Identifier: BSD-2-Clause */
#include "mock_packet.h"
#include <errno.h>
#include <string.h>

static uint16_t load_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void store_be16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static bool is_vlan_tpid(uint16_t type)
{
  return type == 0x8100u || type == 0x88a8u || type == 0x9100u;
}

bool ef_mock_packet_matches_ip4_udp(const uint8_t *frame, size_t length,
                                     const struct ef_mock_ipv4_udp_filter *filter,
                                     bool *is_multicast)
{
  if (is_multicast) *is_multicast = false;
  if (!frame || !filter || length < EFVI_MOCK_ETH_HEADER_BYTES)
    return false;

  size_t offset = EFVI_MOCK_ETH_HEADER_BYTES;
  uint16_t ethertype = load_be16(frame + 12u);
  for (unsigned tag = 0; is_vlan_tpid(ethertype); ++tag) {
    if (tag == EFVI_MOCK_MAX_VLAN_HEADERS || length - offset < 4u)
      return false;
    ethertype = load_be16(frame + offset + 2u);
    offset += 4u;
  }
  if (ethertype != 0x0800u || length - offset < 20u)
    return false;

  const uint8_t *ip = frame + offset;
  size_t ihl = (size_t)(ip[0] & 15u) * 4u;
  if ((ip[0] >> 4) != 4u || ihl < 20u || length - offset < ihl + 8u ||
      ip[9] != 17u)
    return false;

  /* Fragment offsets beyond zero do not contain the UDP header. */
  if ((load_be16(ip + 6u) & 0x1fffu) != 0u)
    return false;
  size_t ip_len = load_be16(ip + 2u);
  if (ip_len < ihl + 8u)
    return false;

  uint32_t dst_be;
  memcpy(&dst_be, ip + 16u, sizeof(dst_be));
  if (filter->address_be && dst_be != filter->address_be)
    return false;

  /* Network-order port is compared as bytes to avoid host-endian mistakes. */
  if (memcmp(ip + ihl + 2u, &filter->port_be, sizeof(filter->port_be)))
    return false;

  if (is_multicast) {
    *is_multicast = ip[16] >= 224u && ip[16] <= 239u;
  }
  return true;
}

int ef_mock_packet_restore_vlan(uint8_t *frame, size_t *length,
                                size_t capacity, uint16_t tpid,
                                uint16_t tci)
{
  if (!frame || !length || *length < EFVI_MOCK_ETH_HEADER_BYTES)
    return -EINVAL;
  if (tpid != 0x8100u && tpid != 0x88a8u && tpid != 0x9100u)
    return -EPROTO;
  if (*length > capacity || capacity - *length < EFVI_MOCK_VLAN_HEADER_BYTES)
    return -ENOBUFS;
  memmove(frame + 16u, frame + 12u, *length - 12u);
  store_be16(frame + 12u, tpid);
  store_be16(frame + 14u, tci);
  *length += EFVI_MOCK_VLAN_HEADER_BYTES;
  return 0;
}
