/* testperf — driver-level throughput measurement for loopback.device.
 *
 * Two openers (A=sender, B=receiver) with distinct BufferManagement.
 * B holds a pool of pre-allocated CMD_READ requests, all pending.
 * A sends N CMD_WRITEs of a chosen payload size back-to-back.
 * We measure wall-clock elapsed with PPC time-base register (mftb).
 *
 * This measures the ceiling of SANA-II copy-hook + dispatch, no
 * Roadshow, no bsdsocket. Compare against virtnet/virte1000 to
 * quantify how much of their throughput ceiling is imposed by
 * the SANA-II layer vs the hardware/QEMU path.
 *
 * Usage: testperf [payload_bytes [iterations]]
 *   defaults: payload=1500, iterations=10000
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <exec/interfaces.h>
#include <devices/sana2.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define POOL_SIZE 16   /* pending CMD_READs on B */

/* Simple memcpy copy-hook. */
static uint32 my_copy_hook(struct Hook *h, APTR unused, struct SANA2CopyHookMsg *m)
{
    (void)h; (void)unused;
    UBYTE *d = (UBYTE *)m->schm_To;
    UBYTE *s = (UBYTE *)m->schm_From;
    for (ULONG i = 0; i < m->schm_Size; i++) d[i] = s[i];
    return 1;
}

static struct Hook g_hook;

/* PPC time base — nominal 100 MHz on sam460ex. */
static inline uint32 tb_low(void)
{
    uint32 t;
    __asm__ volatile ("mftb %0" : "=r"(t) : : "memory");
    return t;
}

int main(int argc, char **argv)
{
    ULONG payload = 1500;
    ULONG iters   = 10000;
    {
        LONG v = 0;
        if (argc >= 2 && IDOS->StrToLong((STRPTR)argv[1], &v) > 0) payload = (ULONG)v;
        if (argc >= 3 && IDOS->StrToLong((STRPTR)argv[2], &v) > 0) iters   = (ULONG)v;
    }
    if (payload > 1514) payload = 1514;
    if (payload < 1)    payload = 1;

    IDOS->Printf("testperf: payload=%lu iterations=%lu pool=%lu\n",
                 payload, iters, (ULONG)POOL_SIZE);
    IDOS->FFlush(IDOS->Output());

    struct MsgPort *pA = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    struct MsgPort *pB = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!pA || !pB) { IDOS->Printf("port alloc fail\n"); return 20; }

    struct IOSana2Req *rA = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, pA, ASOIOR_Size, sizeof(struct IOSana2Req), TAG_END);
    if (!rA) { IDOS->Printf("rA alloc fail\n"); return 20; }

    /* B needs a POOL of ioreqs (each pre-queued as CMD_READ). We use
     * one "primary" request for OpenDevice, then N copies for reads. */
    struct IOSana2Req *rB = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, pB, ASOIOR_Size, sizeof(struct IOSana2Req), TAG_END);
    if (!rB) { IDOS->Printf("rB alloc fail\n"); return 20; }

    g_hook.h_Entry = (uint32(*)())my_copy_hook;
    g_hook.h_SubEntry = NULL; g_hook.h_Data = NULL;

    static struct TagItem tagsA[] = {
        { S2_CopyToBuff, 0 }, { S2_CopyFromBuff, 0 }, { TAG_END, 0 },
    };
    static struct TagItem tagsB[] = {
        { S2_CopyToBuff, 0 }, { S2_CopyFromBuff, 0 }, { TAG_END, 0 },
    };
    tagsA[0].ti_Data = (uint32)&g_hook;
    tagsA[1].ti_Data = (uint32)&g_hook;
    tagsB[0].ti_Data = (uint32)&g_hook;
    tagsB[1].ti_Data = (uint32)&g_hook;
    rA->ios2_BufferManagement = (APTR)tagsA;
    rB->ios2_BufferManagement = (APTR)tagsB;

    LONG e;
    e = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)rA, 0);
    if (e) { IDOS->Printf("open A fail %ld\n", (long)e); return 20; }
    e = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)rB, 0);
    if (e) { IDOS->Printf("open B fail %ld\n", (long)e); return 20; }

    rA->ios2_Req.io_Command = S2_CONFIGINTERFACE;
    for (int i = 0; i < 6; i++) rA->ios2_SrcAddr[i] = 0;
    IExec->DoIO((struct IORequest *)rA);
    rA->ios2_Req.io_Command = S2_ONLINE;
    IExec->DoIO((struct IORequest *)rA);

    /* Build the pool of B's CMD_READ requests. Each needs its own
     * buffer + its own IORequest struct (or we'd race). */
    struct IOSana2Req *pool[POOL_SIZE];
    UBYTE *rx_bufs[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Duplicate, rB,
            TAG_END);
        rx_bufs[i] = (UBYTE *)IExec->AllocVecTags(1600,
            AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_END);
        if (!pool[i] || !rx_bufs[i]) {
            IDOS->Printf("pool alloc %d fail\n", i);
            return 20;
        }
        pool[i]->ios2_BufferManagement = (APTR)tagsB;
        pool[i]->ios2_Req.io_Message.mn_ReplyPort = pB;
        pool[i]->ios2_Req.io_Device = rB->ios2_Req.io_Device;
        pool[i]->ios2_Req.io_Unit   = rB->ios2_Req.io_Unit;
    }

    /* Prime B's queue with all pool requests. */
    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i]->ios2_Req.io_Command = CMD_READ;
        pool[i]->ios2_Req.io_Error   = 0;
        pool[i]->ios2_Data           = rx_bufs[i];
        pool[i]->ios2_DataLength     = 1600;
        pool[i]->ios2_PacketType     = 0x0000;
        IExec->SendIO((struct IORequest *)pool[i]);
    }

    /* TX buffer with a distinctive pattern. */
    static UBYTE tx_buf[1600];
    for (int i = 0; i < 1600; i++) tx_buf[i] = (UBYTE)(0xA0 + (i & 0x1F));

    /* Warmup: 100 iterations to prime caches. */
    ULONG in_flight = POOL_SIZE;
    for (int w = 0; w < 100; w++) {
        rA->ios2_Req.io_Command = CMD_WRITE;
        rA->ios2_Req.io_Error   = 0;
        for (int i = 0; i < 6; i++) rA->ios2_DstAddr[i] = 0xFF;
        rA->ios2_PacketType     = 0x0800;
        rA->ios2_Data           = tx_buf;
        rA->ios2_DataLength     = payload;
        IExec->DoIO((struct IORequest *)rA);

        /* Reap ONE read, re-queue it. */
        struct IOSana2Req *rep = (struct IOSana2Req *)IExec->WaitPort(pB);
        rep = (struct IOSana2Req *)IExec->GetMsg(pB);
        if (rep) {
            rep->ios2_Req.io_Command = CMD_READ;
            rep->ios2_Req.io_Error   = 0;
            rep->ios2_DataLength     = 1600;
            IExec->SendIO((struct IORequest *)rep);
        }
    }
    (void)in_flight;

    /* Measurement loop. */
    uint32 t0 = tb_low();

    for (ULONG n = 0; n < iters; n++) {
        rA->ios2_Req.io_Command = CMD_WRITE;
        rA->ios2_Req.io_Error   = 0;
        for (int i = 0; i < 6; i++) rA->ios2_DstAddr[i] = 0xFF;
        rA->ios2_PacketType     = 0x0800;
        rA->ios2_Data           = tx_buf;
        rA->ios2_DataLength     = payload;
        IExec->DoIO((struct IORequest *)rA);

        /* Reap one; re-queue. */
        (void)IExec->WaitPort(pB);
        struct IOSana2Req *rep = (struct IOSana2Req *)IExec->GetMsg(pB);
        if (rep) {
            rep->ios2_Req.io_Command = CMD_READ;
            rep->ios2_Req.io_Error   = 0;
            rep->ios2_DataLength     = 1600;
            IExec->SendIO((struct IORequest *)rep);
        }
    }

    uint32 t1 = tb_low();
    uint32 elapsed_ticks = t1 - t0;   /* wraps naturally */

    /* TB is 100 MHz — 1 tick = 10 ns. */
    double elapsed_sec = (double)elapsed_ticks / 100000000.0;
    double pps = (double)iters / elapsed_sec;
    double bps = pps * (double)payload;
    double mbps = bps * 8.0 / 1000000.0;

    IDOS->Printf("\n=== results ===\n");
    IDOS->Printf("payload  : %lu bytes\n", payload);
    IDOS->Printf("iters    : %lu\n", iters);
    IDOS->Printf("ticks    : %lu (TB@100MHz)\n", (ULONG)elapsed_ticks);
    IDOS->Printf("elapsed  : %lu ms\n", (ULONG)(elapsed_sec * 1000.0));
    IDOS->Printf("pkts/sec : %lu\n", (ULONG)pps);
    IDOS->Printf("bytes/s  : %lu\n", (ULONG)bps);
    IDOS->Printf("Mbit/s   : %lu.%03lu\n",
                 (ULONG)mbps, (ULONG)((mbps - (double)(ULONG)mbps) * 1000.0));
    IDOS->Printf("per-pkt  : %lu us\n",
                 (ULONG)((elapsed_sec / (double)iters) * 1000000.0));
    IDOS->FFlush(IDOS->Output());

    /* Cleanup — drain any pending. */
    for (int i = 0; i < POOL_SIZE; i++) {
        IExec->AbortIO((struct IORequest *)pool[i]);
        IExec->WaitIO((struct IORequest *)pool[i]);
        IExec->FreeVec(rx_bufs[i]);
        IExec->FreeSysObject(ASOT_IOREQUEST, pool[i]);
    }
    IExec->CloseDevice((struct IORequest *)rA);
    IExec->CloseDevice((struct IORequest *)rB);
    IExec->FreeSysObject(ASOT_IOREQUEST, rA);
    IExec->FreeSysObject(ASOT_IOREQUEST, rB);
    IExec->FreeSysObject(ASOT_PORT, pA);
    IExec->FreeSysObject(ASOT_PORT, pB);
    return 0;
}
