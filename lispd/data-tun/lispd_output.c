/*
 * lispd_output.c
 *
 * This file is part of LISP Mobile Node Implementation.
 *
 * Copyright (C) 2012 Cisco Systems, Inc, 2012. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please send any bug reports or fixes you make to the email address(es):
 *    LISP-MN developers <devel@lispmob.org>
 *
 * Written or modified by:
 *    Alberto Rodriguez Natal <arnatal@ac.upc.edu>
 *    Florin Coras <fcoras@ac.upc.edu>
 */


#include <errno.h>

#include "lispd_output.h"
#include "liblisp.h"
#include "packets.h"
#include "sockets.h"
#include "lispd_info_nat.h"
#include "lisp_control.h"
#include "lispd_tun.h"
#include "lmlog.h"

#include "sockets-util.h"

/* static buffer to receive packets */
static uint8_t pkt_recv_buf[TUN_RECEIVE_SIZE];
static lbuf_t pkt_buf;

static int lisp_output_multicast(lbuf_t *b, packet_tuple_t *tuple);
static int lisp_output_unicast(lbuf_t *b, packet_tuple_t *tuple);
static int forward_native(lbuf_t *b, lisp_addr_t *dst);
static inline int is_lisp_packet(packet_tuple_t *tpl);

static int
forward_native(lbuf_t *b, lisp_addr_t *dst)
{
    int ret = 0, ofd = 0, afi = 0;

    afi = lisp_addr_ip_afi(dst);
    ofd = get_default_output_socket(afi);

    if (ofd == -1) {
        lmlog(DBG_2, "fordward_native: No output interface for afi %d", afi);
        return (BAD);
    }

    lmlog(DBG_3, "Fordwarding native to destination %s",
            lisp_addr_to_char(dst));

    ret = send_raw(ofd, lbuf_data(b), lbuf_size(b), lisp_addr_ip(dst));
    return (ret);
}


static inline int
is_lisp_packet(packet_tuple_t *tpl)
{
    /* Don't encapsulate LISP messages  */
    if (tpl->protocol != IPPROTO_UDP) {
        return (FALSE);
    }

    /* If either of the udp ports are the control port or data, allow
     * to go out natively. This is a quick way around the
     * route filter which rewrites the EID as the source address. */
    if (tpl->dst_port != LISP_CONTROL_PORT
        && tpl->src_port != LISP_CONTROL_PORT
        && tpl->src_port != LISP_DATA_PORT
        && tpl->dst_port != LISP_DATA_PORT) {
        return (FALSE);
    }

    return (TRUE);
}

static int
make_mcast_addr(packet_tuple_t *tuple, lisp_addr_t *addr){

    /* TODO this really needs optimization */

    uint16_t    plen;
    lcaf_addr_t *lcaf;

    if (ip_addr_is_multicast(lisp_addr_ip(&tuple->dst_addr))) {
        if (lisp_addr_afi(&tuple->src_addr) != LM_AFI_IP
            || lisp_addr_afi(&tuple->src_addr) != LM_AFI_IP) {
           lmlog(DBG_1, "tuple_get_dst_lisp_addr: (S,G) (%s, %s)pair is not "
                   "of IP syntax!", lisp_addr_to_char(&tuple->src_addr),
                   lisp_addr_to_char(&tuple->dst_addr));
           return(BAD);
        }

        lisp_addr_set_afi(addr, LM_AFI_LCAF);
        plen = ip_afi_to_default_mask(lisp_addr_ip_afi(&tuple->dst_addr));
        lcaf = lisp_addr_get_lcaf(addr);
        lcaf_addr_set_mc(lcaf, &tuple->src_addr, &tuple->dst_addr, plen, plen,
                0);

    } else {
        lisp_addr_set_afi(addr, LM_AFI_NO_ADDR);
    }

    return(GOOD);
}

static int
lisp_output_multicast(lbuf_t *b, packet_tuple_t *tuple)
{
    glist_t *or_list = NULL;
    uint8_t *encap_packet = NULL;
    lisp_addr_t *src_rloc = NULL, *daddr = NULL, *dst_rloc = NULL;
    locator_t *locator = NULL;
    glist_entry_t *it = NULL;
    int encap_plen = 0;
    int osock = 0;

    lmlog(DBG_1, "Multicast packets not supported for now!");
    return(GOOD);

    /* convert tuple to lisp_addr_t, to be used for map-cache lookup
     * TODO: should be a tad more efficient  */
    daddr = lisp_addr_new();
    if (make_mcast_addr(tuple, daddr) != GOOD) {
        lmlog(LWRN, "lisp_output: Unable to determine "
                "destination address from tuple: src %s dst %s",
                lisp_addr_to_char(&tuple->src_addr),
                lisp_addr_to_char(&tuple->dst_addr));
        return(BAD);
    }

    /* get the output RLOC list */
    /* or_list = re_get_orlist(dst_eid); */
    if (!or_list) {
        return(BAD);
    }

    glist_for_each_entry(it, or_list)
    {
        /* TODO: take locator out, just send mcaddr and out socket */
        locator = (locator_t *) glist_entry_data(it);
        src_rloc = lcaf_mc_get_src(lisp_addr_get_lcaf(locator_addr(locator)));
        dst_rloc = lcaf_mc_get_grp(lisp_addr_get_lcaf(locator_addr(locator)));

        /* FIXME: this works only with RAW sockets */
        lisp_data_encap(b, LISP_DATA_PORT, LISP_DATA_PORT, src_rloc, dst_rloc);

        send_raw(osock, lbuf_data(b), lbuf_size(b), lisp_addr_ip(dst_rloc));

        osock = iface_socket(get_interface_with_address(src_rloc),
                lisp_addr_ip_afi(src_rloc));
        send_packet(osock, encap_packet, encap_plen);

        free(encap_packet);
    }

    glist_destroy(or_list);

    return (GOOD);
}

static int
lisp_output_unicast(lbuf_t *b, packet_tuple_t *tuple)
{
    fwd_entry_t *fwd_entry = NULL;
    int dafi, osock;
    iface_t *iface;

    fwd_entry = ctrl_get_forwarding_entry(tuple);

    /* Packets with no/negative map cache entry AND no PETR
     * OR packets with missing src or dst RLOCs
     * forward them natively */
    if (!fwd_entry || (!fwd_entry->srloc && !fwd_entry->drloc)) {
        return(forward_native(b, &tuple->dst_addr));
    }


    dafi = lisp_addr_ip_afi(fwd_entry->drloc);

    /* if no srloc, choose default */
    if (!fwd_entry->srloc) {
        fwd_entry->srloc = get_default_output_address(dafi);
        if (!fwd_entry->srloc) {
            free(fwd_entry);
            lmlog(DBG_1, "Failed to set source RLOC with afi %d", dafi);
            return(BAD);
        }
    }

    iface = get_interface_with_address(fwd_entry->srloc);
    osock = iface_socket(iface, dafi);

    /* FIXME: this works only with RAW sockets */
    lisp_data_encap(b, LISP_DATA_PORT, LISP_DATA_PORT, fwd_entry->srloc,
            fwd_entry->drloc);

    lmlog(DBG_3,"OUTPUT: Sending encapsulated packet: RLOC %s -> %s\n",
            lisp_addr_to_char(fwd_entry->srloc),
            lisp_addr_to_char(fwd_entry->drloc));

    send_raw(osock, lbuf_data(b), lbuf_size(b),
            lisp_addr_ip(fwd_entry->drloc));

    free(fwd_entry);
    return (GOOD);
}

int
lisp_output(lbuf_t *b)
{
    packet_tuple_t tuple;
    /* fcoras TODO: should use get_dst_lisp_addr instead of tuple */

    if (pkt_parse_5_tuple(b, &tuple) != GOOD) {
        return (BAD);
    }

    lmlog(DBG_3,"OUTPUT: Received EID %s -> %s",
            lisp_addr_to_char(&tuple.src_addr),
            lisp_addr_to_char(&tuple.dst_addr));


    /* If already LISP packet, do not encapsulate again */
    if (is_lisp_packet(&tuple)) {
        return (forward_native(b, &tuple.dst_addr));
    }

    if (ip_addr_is_multicast(lisp_addr_ip(&tuple.dst_addr))) {
        lisp_output_multicast(b, &tuple);
    } else {
        lisp_output_unicast(b, &tuple);
    }

    return(GOOD);
}

int
recv_output_packet(struct sock *sl)
{
    lbuf_use_stack(&pkt_buf, &pkt_recv_buf, TUN_RECEIVE_SIZE);
    lbuf_reserve(&pkt_buf, MAX_LISP_PKT_ENCAP_LEN);

    if (sock_recv(sl->fd, &pkt_buf) != GOOD) {
        lmlog(LWRN, "OUTPUT: Error while reading from tun!");
        return (BAD);
    }
    lbuf_reset_ip(&pkt_buf);
    lisp_output(&pkt_buf);
    return (GOOD);
}

