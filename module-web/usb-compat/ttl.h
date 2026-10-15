/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QDC_USB_TTL_H
#define QDC_USB_TTL_H
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int qdc_usb_cellular_route(const char *in, const char *out)
{
    unsigned int i;
    if (!in || !out || (strcmp(in, "bridge0") && strcmp(in, "ecm0")))
        return 0;
    if (strncmp(out, "rmnet_data", 10) || !out[10])
        return 0;
    for (i = 10; out[i]; i++)
        if (out[i] < '0' || out[i] > '9')
            return 0;
    return 1;
}

/* IPv4 TTL/protocol and checksum are network-order 16-bit words.
 * Update only the TTL and its header checksum; transport checksums do not
 * include TTL. Return zero for an unchanged or invalid header. */
static int qdc_ipv4_ttl64(unsigned char *ip, unsigned int available)
{
    unsigned int hlen, old_word, new_word, checksum, sum;
    if (available < 20 || (ip[0] >> 4) != 4)
        return 0;
    hlen = (ip[0] & 15) * 4;
    if (hlen < 20 || hlen > available || ip[8] == 0 || ip[8] == 64)
        return 0;
    old_word = ((unsigned int)ip[8] << 8) | ip[9];
    new_word = (64U << 8) | ip[9];
    checksum = ((unsigned int)ip[10] << 8) | ip[11];
    sum = ((~checksum) & 65535U) + ((~old_word) & 65535U) + new_word;
    sum = (sum & 65535U) + (sum >> 16);
    sum = (sum & 65535U) + (sum >> 16);
    checksum = (~sum) & 65535U;
    ip[8] = 64;
    ip[10] = checksum >> 8;
    ip[11] = checksum & 255;
    return 1;
}
#endif
