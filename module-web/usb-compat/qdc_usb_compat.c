// SPDX-License-Identifier: GPL-2.0-only
/* Experimental QDC507 USB-to-cellular IPv4 forwarding compatibility.
 * Build with the validated stock kernel tree, not generic Linux headers.
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include "ttl.h"

/* Loading registers the hook but leaves traffic unchanged until enabled. */
static bool enabled;
module_param(enabled, bool, 0600);
MODULE_PARM_DESC(enabled, "Enable USB-to-cellular IPv4 TTL64 compatibility");

static atomic_t adjusted = ATOMIC_INIT(0);
static int qdc_adjusted_get(char *buffer, const struct kernel_param *parameter)
{
    return scnprintf(buffer, 32, "%u", (unsigned int)atomic_read(&adjusted));
}
static const struct kernel_param_ops qdc_adjusted_ops = {
    .get = qdc_adjusted_get,
};
module_param_cb(adjusted, &qdc_adjusted_ops, NULL, 0444);
MODULE_PARM_DESC(adjusted, "Number of adjusted packets, wraps at 32 bits");

static unsigned int qdc_usb_forward(const struct nf_hook_ops *ops,
                                  struct sk_buff *skb,
                                  const struct net_device *in,
                                  const struct net_device *out,
                                  int (*okfn)(struct sk_buff *))
{
    unsigned int offset, length;
    struct iphdr *ip;
    if (!ACCESS_ONCE(enabled) || !skb || !in || !out)
        return NF_ACCEPT;
    if (!qdc_usb_cellular_route(in->name, out->name))
        return NF_ACCEPT;
    offset = skb_network_offset(skb);
    if (offset > skb->len || skb->len - offset < sizeof(struct iphdr) ||
        !pskb_may_pull(skb, offset + sizeof(struct iphdr)))
        return NF_ACCEPT;
    ip = ip_hdr(skb);
    if (ip->version != 4 || ip->ihl < 5 || !ip->ttl || ip->ttl == 64)
        return NF_ACCEPT;
    length = offset + ip->ihl * 4;
    if (length > skb->len || !skb_make_writable(skb, length))
        return NF_ACCEPT;
    /* ip_forward() has already decremented TTL at this hook. Local module
     * traffic uses LOCAL_OUT, so Oray/Bark are outside this path. */
    if (qdc_ipv4_ttl64((unsigned char *)ip_hdr(skb), length - offset))
        atomic_inc(&adjusted);
    return NF_ACCEPT;
}

static struct nf_hook_ops qdc_usb_hook = {
    .hook = qdc_usb_forward,
    .owner = THIS_MODULE,
    .pf = NFPROTO_IPV4,
    .hooknum = NF_INET_FORWARD,
    .priority = NF_IP_PRI_MANGLE,
};

static int __init qdc_usb_init(void)
{
    return nf_register_hook(&qdc_usb_hook);
}

static void __exit qdc_usb_exit(void)
{
    nf_unregister_hook(&qdc_usb_hook);
}

module_init(qdc_usb_init);
module_exit(qdc_usb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QDC507 reversible USB IPv4 forwarding compatibility");
MODULE_VERSION("0.1.0-experimental");
