/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GetNetworkStatistics (LVO -510): a local copy of protocol-stack statistics
 * in the Roadshow NETSTATUS_* struct formats, mapped from lwIP's own
 * counters (lwip_stats plus the MIB-2 counters enabled in lwipopts.h). Many
 * BSD struct fields have no lwIP equivalent and are reported zero, and the
 * mapping is best-effort — documented as approximate in the coverage doc. The
 * struct layouts here are transcribed verbatim from the bsdsocket.doc autodoc
 * so their sizes match the caller's <libraries/bsdsocket.h> definitions.
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>
#include <memory.h>

#include <lwip/ip_addr.h>
#include <lwip/stats.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/priv/tcp_priv.h>

#include "netstack.h"

/* libraries/bsdsocket.h constants */
#define SB_NETWORKSTATUS_VERSION 1
#define SB_NETSTATUS_icmp 0
#define SB_NETSTATUS_igmp 1
#define SB_NETSTATUS_ip 2
#define SB_NETSTATUS_mb 3
#define SB_NETSTATUS_mrt 4
#define SB_NETSTATUS_rt 5
#define SB_NETSTATUS_tcp 6
#define SB_NETSTATUS_udp 7
#define SB_NETSTATUS_tcp_sockets 9
#define SB_NETSTATUS_udp_sockets 10

#define SB_ICMP_MAXTYPE 18 /* netinet/ip_icmp.h */

/* --- BSD stat struct layouts (verbatim field order from the autodoc) ------ */

struct sb_icmpstat
{
    ULONG icps_error, icps_oldshort, icps_oldicmp;
    ULONG icps_outhist[SB_ICMP_MAXTYPE + 1];
    ULONG icps_badcode, icps_tooshort, icps_checksum, icps_badlen, icps_reflect;
    ULONG icps_inhist[SB_ICMP_MAXTYPE + 1];
};

struct sb_igmpstat
{
    ULONG rcv_total, rcv_tooshort, rcv_badsum, rcv_queries, rcv_badqueries;
    ULONG rcv_reports, rcv_badreports, rcv_ourreports, snd_reports;
};

struct sb_ipstat
{
    ULONG ips_total, ips_badsum, ips_tooshort, ips_toosmall, ips_badhlen;
    ULONG ips_badlen, ips_fragments, ips_fragdropped, ips_fragtimeout;
    ULONG ips_forward, ips_cantforward, ips_redirectsent, ips_noproto;
    ULONG ips_delivered, ips_localout, ips_odropped, ips_reassembled;
    ULONG ips_fragmented, ips_ofragments, ips_cantfrag, ips_badoptions;
    ULONG ips_noroute, ips_badvers, ips_rawout;
};

struct sb_mbstat
{
    ULONG m_mbufs, m_clusters, m_spare, m_clfree, m_drops, m_wait, m_drain;
};

struct sb_mrtstat
{
    ULONG mrts_mrt_lookups, mrts_mrt_misses, mrts_grp_lookups, mrts_grp_misses;
    ULONG mrts_no_route, mrts_bad_tunnel, mrts_cant_tunnel, mrts_wrong_if;
};

struct sb_rtstat
{
    WORD rts_badredirect, rts_dynamic, rts_newgateway, rts_unreach, rts_wildcard;
};

struct sb_tcpstat
{
    ULONG tcps_connattempt, tcps_accepts, tcps_connects, tcps_drops;
    ULONG tcps_conndrops, tcps_closed, tcps_segstimed, tcps_rttupdated;
    ULONG tcps_delack, tcps_timeoutdrop, tcps_rexmttimeo, tcps_persisttimeo;
    ULONG tcps_keeptimeo, tcps_keepprobe, tcps_keepdrops;
    ULONG tcps_sndtotal, tcps_sndpack, tcps_sndbyte, tcps_sndrexmitpack;
    ULONG tcps_sndrexmitbyte, tcps_sndacks, tcps_sndprobe, tcps_sndurg;
    ULONG tcps_sndwinup, tcps_sndctrl;
    ULONG tcps_rcvtotal, tcps_rcvpack, tcps_rcvbyte, tcps_rcvbadsum;
    ULONG tcps_rcvbadoff, tcps_rcvshort, tcps_rcvduppack, tcps_rcvdupbyte;
    ULONG tcps_rcvpartduppack, tcps_rcvpartdupbyte, tcps_rcvoopack;
    ULONG tcps_rcvoobyte, tcps_rcvpackafterwin, tcps_rcvbyteafterwin;
    ULONG tcps_rcvafterclose, tcps_rcvwinprobe, tcps_rcvdupack;
    ULONG tcps_rcvacktoomuch, tcps_rcvackpack, tcps_rcvackbyte, tcps_rcvwinupd;
    ULONG tcps_pawsdrop, tcps_predack, tcps_preddat, tcps_pcbcachemiss;
    ULONG tcps_persistdrop, tcps_badsyn;
};

struct sb_udpstat
{
    ULONG udps_ipackets, udps_hdrops, udps_badsum, udps_badlen, udps_noport;
    ULONG udps_noportbcast, udps_fullsock, udpps_pcbcachemiss, udps_opackets;
};

/* protocol_connection_data (24 bytes per the autodoc; struct in_addr wraps a
 * single ULONG) */
struct sb_in_addr
{
    ULONG s_addr;
};

struct sb_pcd
{
    struct sb_in_addr pcd_foreign_address;
    UWORD pcd_foreign_port;
    struct sb_in_addr pcd_local_address;
    UWORD pcd_local_port;
    ULONG pcd_receive_queue_size;
    ULONG pcd_send_queue_size;
    LONG pcd_tcp_state;
};

/* copy min(size, total) bytes out; a NULL destination just queries the size */
static LONG sb_stat_out(APTR destination, LONG size, const void *src, LONG total)
{
    if (destination == NULL)
        return total;
    LONG n = size < total ? size : total;
    if (n < 0)
        n = 0;
    CopyMem((APTR)src, destination, (ULONG)n);
    return n;
}

/* --- per-protocol fills (all read lwip_stats under the core lock) --------- */

static void sb_fill_ip(struct sb_ipstat *st)
{
    memset(st, 0, sizeof(*st));
    st->ips_total = lwip_stats.mib2.ipinreceives;
    st->ips_badsum = lwip_stats.ip.chkerr;
    st->ips_badlen = lwip_stats.ip.lenerr;
    st->ips_fragments = lwip_stats.mib2.ipreasmreqds;
    st->ips_fragdropped = lwip_stats.mib2.ipreasmfails;
    st->ips_forward = lwip_stats.mib2.ipforwdatagrams;
    st->ips_noproto = lwip_stats.mib2.ipinunknownprotos;
    st->ips_delivered = lwip_stats.mib2.ipindelivers;
    st->ips_localout = lwip_stats.mib2.ipoutrequests;
    st->ips_odropped = lwip_stats.mib2.ipoutdiscards;
    st->ips_reassembled = lwip_stats.mib2.ipreasmoks;
    st->ips_fragmented = lwip_stats.mib2.ipfragoks;
    st->ips_ofragments = lwip_stats.mib2.ipfragcreates;
    st->ips_cantfrag = lwip_stats.mib2.ipfragfails;
    st->ips_badoptions = lwip_stats.ip.opterr;
    st->ips_noroute = lwip_stats.mib2.ipoutnoroutes;
}

static void sb_fill_tcp(struct sb_tcpstat *st)
{
    memset(st, 0, sizeof(*st));
    st->tcps_connattempt = lwip_stats.mib2.tcpactiveopens;
    st->tcps_accepts = lwip_stats.mib2.tcppassiveopens;
    st->tcps_conndrops = lwip_stats.mib2.tcpattemptfails;
    st->tcps_drops = lwip_stats.mib2.tcpestabresets;
    st->tcps_sndtotal = lwip_stats.mib2.tcpoutsegs;
    st->tcps_sndrexmitpack = lwip_stats.mib2.tcpretranssegs;
    st->tcps_rcvtotal = lwip_stats.mib2.tcpinsegs;
    st->tcps_rcvbadsum = lwip_stats.tcp.chkerr;
    st->tcps_rcvshort = lwip_stats.tcp.lenerr;
    st->tcps_sndctrl = lwip_stats.mib2.tcpoutrsts; /* RSTs only (partial) */
}

static void sb_fill_udp(struct sb_udpstat *st, ULONG fullsock)
{
    memset(st, 0, sizeof(*st));
    st->udps_ipackets = lwip_stats.mib2.udpindatagrams;
    st->udps_noport = lwip_stats.mib2.udpnoports;
    st->udps_badsum = lwip_stats.udp.chkerr;
    st->udps_badlen = lwip_stats.udp.lenerr;
    st->udps_hdrops = lwip_stats.mib2.udpinerrors;
    st->udps_opackets = lwip_stats.mib2.udpoutdatagrams;
    st->udps_fullsock = fullsock; /* datagrams dropped at our socket queues */
}

static void sb_fill_icmp(struct sb_icmpstat *st)
{
    memset(st, 0, sizeof(*st));
    const struct stats_mib2 *m = &lwip_stats.mib2;
    st->icps_checksum = m->icmpinerrors; /* closest available approximation */
    st->icps_inhist[8] = m->icmpinechos;
    st->icps_inhist[0] = m->icmpinechoreps;
    st->icps_inhist[3] = m->icmpindestunreachs;
    st->icps_inhist[4] = m->icmpinsrcquenchs;
    st->icps_inhist[5] = m->icmpinredirects;
    st->icps_inhist[11] = m->icmpintimeexcds;
    st->icps_inhist[12] = m->icmpinparmprobs;
    st->icps_inhist[13] = m->icmpintimestamps;
    st->icps_inhist[14] = m->icmpintimestampreps;
    st->icps_inhist[17] = m->icmpinaddrmasks;
    st->icps_inhist[18] = m->icmpinaddrmaskreps;
    st->icps_outhist[8] = m->icmpoutechos;
    st->icps_outhist[0] = m->icmpoutechoreps;
    st->icps_outhist[3] = m->icmpoutdestunreachs;
    st->icps_outhist[11] = m->icmpouttimeexcds;
}

static void sb_fill_igmp(struct sb_igmpstat *st)
{
    memset(st, 0, sizeof(*st));
#if IGMP_STATS
    st->rcv_total = lwip_stats.igmp.recv;
    st->rcv_tooshort = lwip_stats.igmp.lenerr;
    st->rcv_badsum = lwip_stats.igmp.chkerr;
    st->rcv_queries = lwip_stats.igmp.rx_group + lwip_stats.igmp.rx_general;
    st->rcv_reports = lwip_stats.igmp.rx_report;
    st->snd_reports = lwip_stats.igmp.tx_report + lwip_stats.igmp.tx_join +
                      lwip_stats.igmp.tx_leave;
#endif
}

/* --- connection tables (NETSTATUS_tcp_sockets / _udp_sockets) ------------- */

/* lwIP tcp_state -> BSD tcp_fsm.h state (they diverge from index 5 up) */
static const LONG sb_bsd_tcp_state[11] = {0, 1, 2, 3, 4, 6, 9, 5, 7, 8, 10};

/* Emit one pcd entry into out (if it fits within remaining), always counting.
 * *written accumulates bytes actually copied. */
static void sb_pcd_emit(UBYTE *out, LONG size, LONG *written, LONG *count,
                        ULONG faddr, UWORD fport, ULONG laddr, UWORD lport,
                        ULONG rq, ULONG sq, LONG state)
{
    (*count)++;
    if (out == NULL || *written + (LONG)sizeof(struct sb_pcd) > size)
        return;
    struct sb_pcd pcd;
    pcd.pcd_foreign_address.s_addr = faddr;
    pcd.pcd_foreign_port = fport;
    pcd.pcd_local_address.s_addr = laddr;
    pcd.pcd_local_port = lport;
    pcd.pcd_receive_queue_size = rq;
    pcd.pcd_send_queue_size = sq;
    pcd.pcd_tcp_state = state;
    CopyMem(&pcd, out + *written, sizeof(pcd));
    *written += (LONG)sizeof(pcd);
}

static LONG sb_tcp_sockets(APTR destination, LONG size)
{
    UBYTE *out = destination;
    LONG written = 0, count = 0;

    for (struct tcp_pcb *p = tcp_active_pcbs; p != NULL; p = p->next)
    {
        struct SbSocket *s = (struct SbSocket *)p->callback_arg;
        ULONG rq = (s != NULL) ? s->rxBytes : 0;
        /* raw snd_buf, NOT the tcp_sndbuf() macro — that one is TCPWND16-
         * clamped to 64 KB and would inflate an idle socket's queue by ~983 KB
         * against the 1 MB TCP_SND_BUF */
        ULONG sq = (ULONG)TCP_SND_BUF - p->snd_buf;
        LONG state = (p->state >= 0 && p->state <= 10) ? sb_bsd_tcp_state[p->state] : -1;
        sb_pcd_emit(out, size, &written, &count,
                    ip4_addr_get_u32(ip_2_ip4(&p->remote_ip)), p->remote_port,
                    ip4_addr_get_u32(ip_2_ip4(&p->local_ip)), p->local_port,
                    rq, sq, state);
    }
    for (struct tcp_pcb *p = tcp_tw_pcbs; p != NULL; p = p->next)
    {
        sb_pcd_emit(out, size, &written, &count,
                    ip4_addr_get_u32(ip_2_ip4(&p->remote_ip)), p->remote_port,
                    ip4_addr_get_u32(ip_2_ip4(&p->local_ip)), p->local_port,
                    0, 0, sb_bsd_tcp_state[TIME_WAIT]);
    }
    for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p != NULL; p = p->next)
    {
        sb_pcd_emit(out, size, &written, &count,
                    0, 0,
                    ip4_addr_get_u32(ip_2_ip4(&p->local_ip)), p->local_port,
                    0, 0, sb_bsd_tcp_state[LISTEN]);
    }
    return destination == NULL ? count * (LONG)sizeof(struct sb_pcd) : written;
}

static LONG sb_udp_sockets(APTR destination, LONG size)
{
    UBYTE *out = destination;
    LONG written = 0, count = 0;

    for (struct udp_pcb *p = udp_pcbs; p != NULL; p = p->next)
    {
        sb_pcd_emit(out, size, &written, &count,
                    ip4_addr_get_u32(ip_2_ip4(&p->remote_ip)), p->remote_port,
                    ip4_addr_get_u32(ip_2_ip4(&p->local_ip)), p->local_port,
                    0, 0, -1); /* TCP state is meaningless for UDP */
    }
    return destination == NULL ? count * (LONG)sizeof(struct sb_pcd) : written;
}

LONG bsd_GetNetworkStatistics(LONG type asm("d0"), LONG version asm("d1"),
                              APTR destination asm("a0"), LONG size asm("d2"),
                              struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: type=%ld version=%ld dest=0x%08lx size=%ld\n",
             __func__, type, version, (ULONG)destination, size);
    if (version != SB_NETWORKSTATUS_VERSION)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    LONG r;
    netstack_lock();
    switch (type)
    {
    case SB_NETSTATUS_ip:
    {
        struct sb_ipstat st;
        sb_fill_ip(&st);
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_tcp:
    {
        struct sb_tcpstat st;
        sb_fill_tcp(&st);
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_udp:
    {
        struct sb_udpstat st;
        sb_fill_udp(&st, SB_ROOT(base)->dgramRxDrops);
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_icmp:
    {
        struct sb_icmpstat st;
        sb_fill_icmp(&st);
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_igmp:
    {
        struct sb_igmpstat st;
        sb_fill_igmp(&st);
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_mb:
    {
        struct sb_mbstat st; /* no lwIP equivalent — zero-filled */
        memset(&st, 0, sizeof(st));
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_mrt:
    {
        struct sb_mrtstat st; /* not a multicast router — zero-filled */
        memset(&st, 0, sizeof(st));
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_rt:
    {
        struct sb_rtstat st; /* no redirect/route-cache stats — zero-filled */
        memset(&st, 0, sizeof(st));
        r = sb_stat_out(destination, size, &st, sizeof(st));
        break;
    }
    case SB_NETSTATUS_tcp_sockets:
        r = sb_tcp_sockets(destination, size);
        break;
    case SB_NETSTATUS_udp_sockets:
        r = sb_udp_sockets(destination, size);
        break;
    default:
        netstack_unlock();
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    netstack_unlock();
    return r;
}
