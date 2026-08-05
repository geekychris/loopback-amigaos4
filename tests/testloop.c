/* testloop — verifies TX→RX loopback semantics end-to-end.
 *
 * Opens the device TWICE (two openers), configures each with the
 * classic S2_CopyToBuff / S2_CopyFromBuff tag pointing at a plain
 * memcpy-style hook. Opener B queues a CMD_READ. Opener A sends a
 * CMD_WRITE with a distinctive 60-byte payload. The driver's
 * loopback delivery should hand it to opener B; we verify the
 * received bytes match the sent bytes.
 *
 * This bypasses Roadshow — pure driver-level round-trip test. */

#include <exec/errors.h>
#include <exec/io.h>
#include <exec/interfaces.h>
#include <devices/sana2.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../include/loopback.h"

/* Classic-style CopyToBuff / CopyFromBuff: hook that just memcpys.
 * SANA-II schm_Method dispatch uses the tag ID as the "which method"
 * indicator; the hook fn checks the message's method. */
static uint32 my_copy_hook(struct Hook *h, APTR unused, struct SANA2CopyHookMsg *m)
{
    (void)h; (void)unused;
    UBYTE *d = (UBYTE *)m->schm_To;
    UBYTE *s = (UBYTE *)m->schm_From;
    for (ULONG i = 0; i < m->schm_Size; i++) d[i] = s[i];
    return 1;
}

static struct Hook g_hook;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *pA = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    struct MsgPort *pB = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!pA || !pB) return 20;

    struct IOSana2Req *rA = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, pA, ASOIOR_Size, sizeof(struct IOSana2Req), TAG_END);
    struct IOSana2Req *rB = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, pB, ASOIOR_Size, sizeof(struct IOSana2Req), TAG_END);
    if (!rA || !rB) return 20;

    /* Register the same hook on both openers via BufferManagement tag list. */
    g_hook.h_Entry = (uint32(*)())my_copy_hook;
    g_hook.h_SubEntry = NULL; g_hook.h_Data = NULL;

    /* IMPORTANT: each opener needs its OWN BufferManagement pointer.
     * The driver uses this pointer as the opener's identity for
     * dispatching CMD_READ/WRITE. If two openers share the pointer,
     * lookups collide on the first-added opener. Roadshow naturally
     * hands each interface a distinct tag list. */
    static struct TagItem tagsA[] = {
        { S2_CopyToBuff,   (uint32)&g_hook },
        { S2_CopyFromBuff, (uint32)&g_hook },
        { TAG_END,         0 },
    };
    static struct TagItem tagsB[] = {
        { S2_CopyToBuff,   (uint32)&g_hook },
        { S2_CopyFromBuff, (uint32)&g_hook },
        { TAG_END,         0 },
    };
    rA->ios2_BufferManagement = (APTR)tagsA;
    rB->ios2_BufferManagement = (APTR)tagsB;

    LONG e;
    IDOS->Printf("step 1: open A\n"); IDOS->FFlush(IDOS->Output());
    e = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)rA, 0);
    if (e) { IDOS->Printf("open A fail %ld\n", (long)e); return 20; }
    IDOS->Printf("step 2: open B\n"); IDOS->FFlush(IDOS->Output());
    e = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)rB, 0);
    if (e) { IDOS->Printf("open B fail %ld\n", (long)e); return 20; }

    IDOS->Printf("step 3: S2_CONFIGINTERFACE\n"); IDOS->FFlush(IDOS->Output());
    rA->ios2_Req.io_Command = S2_CONFIGINTERFACE;
    for (int i = 0; i < 6; i++) rA->ios2_SrcAddr[i] = 0;
    IExec->DoIO((struct IORequest *)rA);
    IDOS->Printf("  CONFIGINTERFACE returned: ioerr=%ld wire=0x%lx\n",
                 (long)rA->ios2_Req.io_Error,
                 (unsigned long)rA->ios2_WireError);
    IDOS->FFlush(IDOS->Output());
    IDOS->Printf("step 4: S2_ONLINE\n"); IDOS->FFlush(IDOS->Output());
    rA->ios2_Req.io_Command = S2_ONLINE;
    IExec->DoIO((struct IORequest *)rA);
    IDOS->Printf("  ONLINE returned: ioerr=%ld wire=0x%lx\n",
                 (long)rA->ios2_Req.io_Error,
                 (unsigned long)rA->ios2_WireError);
    IDOS->FFlush(IDOS->Output());

    /* Opener B queues a CMD_READ with a buffer big enough for our test frame. */
    static UBYTE rx_buf[128];
    for (int i = 0; i < 128; i++) rx_buf[i] = 0;
    IDOS->Printf("step 5: SendIO CMD_READ on B\n"); IDOS->FFlush(IDOS->Output());
    rB->ios2_Req.io_Command = CMD_READ;
    rB->ios2_Req.io_Error   = 0;
    rB->ios2_Data           = rx_buf;
    rB->ios2_DataLength     = 60;
    rB->ios2_PacketType     = 0x0000;   /* accept any type */
    IExec->SendIO((struct IORequest *)rB);
    IDOS->Printf("  SendIO returned\n"); IDOS->FFlush(IDOS->Output());

    /* Opener A sends a CMD_WRITE with distinctive payload. */
    static UBYTE tx_buf[60];
    for (int i = 0; i < 60; i++) tx_buf[i] = (UBYTE)(0xA0 + (i & 0x1F));
    rA->ios2_Req.io_Command = CMD_WRITE;
    rA->ios2_Req.io_Error   = 0;
    for (int i = 0; i < 6; i++) rA->ios2_DstAddr[i] = 0xFF;   /* broadcast */
    rA->ios2_PacketType     = 0x0800;
    rA->ios2_Data           = tx_buf;
    rA->ios2_DataLength     = 60;
    IDOS->Printf("step 6: DoIO CMD_WRITE on A\n"); IDOS->FFlush(IDOS->Output());
    LONG rc = IExec->DoIO((struct IORequest *)rA);
    IDOS->Printf("CMD_WRITE rc=%ld ioerr=%ld wire=0x%lx\n",
                 (long)rc, (long)rA->ios2_Req.io_Error,
                 (unsigned long)rA->ios2_WireError);
    IDOS->FFlush(IDOS->Output());

    /* Now B's CMD_READ should be replied. Wait for it. */
    ULONG got = IExec->Wait((1UL << pB->mp_SigBit) | SIGBREAKF_CTRL_C);
    if (got & SIGBREAKF_CTRL_C) { IDOS->Printf("CTRL-C\n"); goto out; }
    struct IOSana2Req *reply = (struct IOSana2Req *)IExec->GetMsg(pB);
    if (!reply) { IDOS->Printf("no reply from B\n"); goto out; }
    IDOS->Printf("RX  ioerr=%ld  len=%lu  ptype=0x%04lx\n",
                 (long)reply->ios2_Req.io_Error,
                 (unsigned long)reply->ios2_DataLength,
                 (unsigned long)reply->ios2_PacketType);

    /* Byte-check. */
    BOOL match = TRUE;
    for (int i = 0; i < 60; i++) {
        if (rx_buf[i] != tx_buf[i]) { match = FALSE; break; }
    }
    IDOS->Printf("payload match: %s\n", match ? "YES" : "NO");
    IDOS->Printf("first 16 bytes rx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7],
        rx_buf[8], rx_buf[9], rx_buf[10], rx_buf[11], rx_buf[12], rx_buf[13], rx_buf[14], rx_buf[15]);

out:
    IExec->CloseDevice((struct IORequest *)rA);
    IExec->CloseDevice((struct IORequest *)rB);
    IExec->FreeSysObject(ASOT_IOREQUEST, rA);
    IExec->FreeSysObject(ASOT_IOREQUEST, rB);
    IExec->FreeSysObject(ASOT_PORT, pA);
    IExec->FreeSysObject(ASOT_PORT, pB);
    return 0;
}
