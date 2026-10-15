/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ttl.h"

/* Independent full-header checksum verifies the incremental adjustment. */
static unsigned int checksum(const unsigned char *p, unsigned int length)
{
    unsigned int sum = 0, i;
    for (i = 0; i < length; i += 2)
        sum += ((unsigned int)p[i] << 8) | p[i + 1];
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return (~sum) & 65535;
}

int main(void)
{
    unsigned char ip[60], original[60];
    unsigned int words, ttl, protocol, check;
    assert(qdc_usb_cellular_route("bridge0", "rmnet_data0"));
    assert(qdc_usb_cellular_route("ecm0", "rmnet_data12"));
    assert(!qdc_usb_cellular_route(NULL, "rmnet_data0"));
    assert(!qdc_usb_cellular_route("bridge0", NULL));
    assert(!qdc_usb_cellular_route("lo", "rmnet_data0"));
    assert(!qdc_usb_cellular_route("rmnet_data0", "bridge0"));
    assert(!qdc_usb_cellular_route("bridge0", "eth0"));
    assert(!qdc_usb_cellular_route("bridge0", "rmnet_data"));
    assert(!qdc_usb_cellular_route("bridge0", "rmnet_database"));
    for (words = 5; words <= 15; words++)
        for (ttl = 1; ttl <= 255; ttl++)
            for (protocol = 1; protocol <= 255; protocol++) {
                unsigned int i;
                for (i = 0; i < sizeof(ip); i++)
                    ip[i] = (i * 37 + protocol) & 255;
                ip[0] = 0x40 | words;
                ip[8] = ttl;
                ip[9] = protocol;
                ip[10] = ip[11] = 0;
                check = checksum(ip, words * 4);
                ip[10] = check >> 8;
                ip[11] = check & 255;
                memcpy(original, ip, sizeof(ip));
                assert(qdc_ipv4_ttl64(ip, words * 4) == (ttl != 64));
                assert(ip[8] == 64);
                assert(checksum(ip, words * 4) == 0);
                for (i = 0; i < sizeof(ip); i++)
                    if (i != 8 && i != 10 && i != 11)
                        assert(ip[i] == original[i]);
            }
    memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    ip[8] = 63;
    assert(!qdc_ipv4_ttl64(ip, 19));
    ip[0] = 0x44;
    assert(!qdc_ipv4_ttl64(ip, 20));
    ip[0] = 0x4f;
    assert(!qdc_ipv4_ttl64(ip, 59));
    ip[0] = 0x65;
    assert(!qdc_ipv4_ttl64(ip, 60));
    ip[0] = 0x45;
    ip[8] = 0;
    assert(!qdc_ipv4_ttl64(ip, 60));
    puts("IPv4 TTL checksum/options/preservation checks passed");
    return 0;
}
