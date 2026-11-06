/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef EFVI_MOCK_PACKET_H
#define EFVI_MOCK_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An Ethernet frame is supplied verbatim unless the Linux packet socket
 * reports an accelerated/stripped outer VLAN header in PACKET_AUXDATA. */
#define EFVI_MOCK_ETH_HEADER_BYTES 14u
#define EFVI_MOCK_VLAN_HEADER_BYTES 4u
#define EFVI_MOCK_MAX_VLAN_HEADERS 4u

struct ef_mock_ipv4_udp_filter {
  uint32_t address_be; /* network byte order, in_addr.s_addr representation */
  uint16_t port_be;    /* network byte order, in_port_t representation */
};

bool ef_mock_packet_matches_ip4_udp(const uint8_t *frame, size_t length,
                                     const struct ef_mock_ipv4_udp_filter *filter,
                                     bool *is_multicast);

/* Insert the stripped OUTER VLAN tag before the existing EtherType.  A zero
 * TCI is valid when the auxiliary status has TP_STATUS_VLAN_VALID set. */
int ef_mock_packet_restore_vlan(uint8_t *frame, size_t *length,
                                size_t capacity, uint16_t tpid,
                                uint16_t tci);

#endif
