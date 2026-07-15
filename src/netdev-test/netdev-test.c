/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * netdev-test — first-light bring-up for the netdev vertical slice.
 *
 * Attaches the stack core to genet.device over the netdev ABI, brings the
 * interface up with DHCP (default) or a static address, and runs the timer
 * loop. lwIP answers ARP and ICMP echo by itself, so a successful test is:
 *
 *   > netdev-test              (DHCP; prints the acquired address)
 *   > netdev-test 192.168.1.42 [255.255.255.0 [192.168.1.1]]
 *   ... then ping the address from another host. Ctrl-C exits cleanly
 *   (STOP/DETACH — a DETACH failure means leaked RX buffers, i.e. a bug).
 *
 * This is a diagnostic, not a product: the bsdsocket.library socket layer
 * (Phase 2) replaces it for real use.
 */

#include <stdio.h>
#include <string.h>

#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/types.h>

#include <proto/exec.h>

#include <debug.h>

#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <netdev.h>
#include <netdev_if.h>
#include <netstack.h>

static struct NetdevIf ndi;

static BYTE netdev_cmd(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    Kprintf("[netdev-test] %s: cmd=0x%04lx len=%lu\n", __func__, (ULONG)cmd, len);
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    DoIO((struct IORequest *)io);
    return io->io_Error;
}

int main(int argc, char **argv)
{
    Kprintf("[netdev-test] %s: argc=%ld\n", __func__, (LONG)argc);
    int rc = 20;
    BOOL use_dhcp = (argc < 2);
    BOOL attached = FALSE, started = FALSE, netif_created = FALSE, dev_open = FALSE;

    /* --- timer: netstack time source + the 100 ms tick --- */
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *tick = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
    if (tick == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &tick->tr_node, 0) != 0)
    {
        printf("netdev-test: no timer.device\n");
        goto out;
    }

    netstack_init(tick->tr_node.io_Device);

    /* --- the NIC --- */
    struct MsgPort *devPort = CreateMsgPort();
    struct IOStdReq *dio = (struct IOStdReq *)CreateIORequest(devPort, sizeof(struct IOStdReq));
    if (dio == NULL || OpenDevice((CONST_STRPTR) "genet.device", 0, (struct IORequest *)dio, 0) != 0)
    {
        printf("netdev-test: cannot open genet.device\n");
        goto out_timer;
    }
    dev_open = TRUE;

    struct NetDevAttach att;
    memset(&att, 0, sizeof(att));
    att.nda_AbiVersion = NETDEV_ABI_VERSION;
    att.nda_StackCtx = &ndi;
    att.nda_StackOps = netdevif_stack_ops();

    BYTE err = netdev_cmd(dio, NETDEV_CMD_ATTACH, &att, sizeof(att));
    if (err != 0)
    {
        printf("netdev-test: ATTACH failed (%d)\n", err);
        goto out_dev;
    }
    attached = TRUE;

    const struct NetDevCaps *caps = &att.nda_Caps;
    printf("attached: ABI v%u, MAC %02x:%02x:%02x:%02x:%02x:%02x, MTU %u,\n"
           "  features 0x%08lx, rings tx/rx %u/%u, max segs %u\n",
           att.nda_AbiVersion,
           caps->ndc_Mac[0], caps->ndc_Mac[1], caps->ndc_Mac[2],
           caps->ndc_Mac[3], caps->ndc_Mac[4], caps->ndc_Mac[5],
           caps->ndc_Mtu, (unsigned long)caps->ndc_Features,
           caps->ndc_TxRingSlots, caps->ndc_RxRingSlots, caps->ndc_TxMaxSegs);

    if (netdevif_create(&ndi, att.nda_DrvCtx, att.nda_DrvOps, caps) != 0)
    {
        printf("netdev-test: netdevif_create failed\n");
        goto out_detach;
    }
    netif_created = TRUE;

    /* --- interface configuration --- */
    netstack_lock();
    if (!use_dhcp)
    {
        ip4_addr_t ip, mask, gw;
        ip4_addr_set_zero(&ip);
        ip4_addr_set_zero(&gw);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        if (!ip4addr_aton(argv[1], &ip) ||
            (argc > 2 && !ip4addr_aton(argv[2], &mask)) ||
            (argc > 3 && !ip4addr_aton(argv[3], &gw)))
        {
            netstack_unlock();
            printf("usage: netdev-test [ip [mask [gateway]]]  (no args = DHCP)\n");
            goto out_destroy;
        }
        netif_set_addr(&ndi.ndi_Netif, &ip, &mask, &gw);
    }
    netif_set_default(&ndi.ndi_Netif);
    netif_set_up(&ndi.ndi_Netif);
    netstack_unlock();

    err = netdev_cmd(dio, NETDEV_CMD_START, NULL, 0);
    if (err != 0)
    {
        printf("netdev-test: START failed (%d)\n", err);
        goto out_destroy;
    }
    started = TRUE;

    if (use_dhcp)
    {
        netstack_lock();
        dhcp_start(&ndi.ndi_Netif);
        netstack_unlock();
        printf("DHCP running...\n");
    }

    printf("up — ping me from another host; Ctrl-C to quit\n");

    /* --- the tick loop: lwIP timeouts every 100 ms --- */
    tick->tr_node.io_Command = TR_ADDREQUEST;
    tick->tr_time.tv_secs = 0;
    tick->tr_time.tv_micro = 100000;
    SendIO(&tick->tr_node);

    BOOL addr_reported = !use_dhcp;
    for (;;)
    {
        ULONG sigs = Wait((1UL << timerPort->mp_SigBit) | SIGBREAKF_CTRL_C);

        if (sigs & (1UL << timerPort->mp_SigBit))
        {
            if (CheckIO(&tick->tr_node))
                WaitIO(&tick->tr_node);

            netstack_tick();

            if (!addr_reported && !ip4_addr_isany_val(*netif_ip4_addr(&ndi.ndi_Netif)))
            {
                char buf[IP4ADDR_STRLEN_MAX];
                ip4addr_ntoa_r(netif_ip4_addr(&ndi.ndi_Netif), buf, sizeof(buf));
                printf("DHCP bound: %s\n", buf);
                addr_reported = TRUE;
            }

            tick->tr_node.io_Command = TR_ADDREQUEST;
            tick->tr_time.tv_secs = 0;
            tick->tr_time.tv_micro = 100000;
            SendIO(&tick->tr_node);
        }

        if (sigs & SIGBREAKF_CTRL_C)
            break;
    }

    AbortIO(&tick->tr_node);
    WaitIO(&tick->tr_node);
    rc = 0;

    /* --- teardown; DETACH doubles as the leak check --- */
    netstack_lock();
    if (use_dhcp)
        dhcp_release_and_stop(&ndi.ndi_Netif);
    netif_set_down(&ndi.ndi_Netif);
    netstack_unlock();

out_destroy:
    if (started)
    {
        err = netdev_cmd(dio, NETDEV_CMD_STOP, NULL, 0);
        if (err != 0)
            printf("netdev-test: STOP failed (%d)\n", err);
    }
    if (netif_created)
        netdevif_destroy(&ndi);

out_detach:
    if (attached)
    {
        err = netdev_cmd(dio, NETDEV_CMD_DETACH, NULL, 0);
        if (err != 0)
        {
            printf("netdev-test: DETACH failed (%d) — RX buffers leaked?\n", err);
            rc = 10;
        }
    }

out_dev:
    if (dev_open)
        CloseDevice((struct IORequest *)dio);
    if (dio != NULL)
        DeleteIORequest((struct IORequest *)dio);
    if (devPort != NULL)
        DeleteMsgPort(devPort);

out_timer:
    CloseDevice(&tick->tr_node);
out:
    if (tick != NULL)
        DeleteIORequest(&tick->tr_node);
    if (timerPort != NULL)
        DeleteMsgPort(timerPort);
    return rc;
}
