/* teststat — read the LO_DBG_STATUS counters + derived rates. */
#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../include/loopback.h"

/* TB freq nominal 100 MHz on sam460ex (matches virtnet convention). */
#define TB_HZ 100000000UL

static ULONG be32(UBYTE *p) {
    return ((ULONG)p[0]<<24) | ((ULONG)p[1]<<16) | ((ULONG)p[2]<<8) | (ULONG)p[3];
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *p = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!p) return 20;
    struct IOSana2Req *r = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, p,
        ASOIOR_Size,      sizeof(struct IOSana2Req),
        TAG_END);
    if (!r) { IExec->FreeSysObject(ASOT_PORT, p); return 20; }

    LONG err = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)r, 0);
    if (err) {
        IDOS->Printf("OpenDevice loopback failed: %ld\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, r);
        IExec->FreeSysObject(ASOT_PORT, p);
        return 20;
    }

    UBYTE buf[40] = {0};
    r->ios2_Req.io_Command = LO_DBG_STATUS;
    r->ios2_Data           = buf;
    r->ios2_DataLength     = 40;
    IExec->DoIO((struct IORequest *)r);

    ULONG state  = be32(buf +  0);
    ULONG ptx    = be32(buf +  4);
    ULONG prx    = be32(buf +  8);
    ULONG btx    = be32(buf + 12);
    ULONG brx    = be32(buf + 16);
    ULONG drops  = be32(buf + 20);
    ULONG calls  = be32(buf + 24);
    ULONG ctot   = be32(buf + 28);
    ULONG ccook  = be32(buf + 32);
    ULONG cdlv   = be32(buf + 36);

    IDOS->Printf("--- loopback.device ---\n");
    IDOS->Printf("  state           %lu (0=OFFLINE 1=CONFIGURED 2=ONLINE)\n", state);
    IDOS->Printf("  packets_tx      %lu\n", ptx);
    IDOS->Printf("  packets_rx      %lu\n", prx);
    IDOS->Printf("  bytes_tx        %lu\n", btx);
    IDOS->Printf("  bytes_rx        %lu\n", brx);
    IDOS->Printf("  drops_no_reader %lu\n", drops);
    if (calls) {
        IDOS->Printf("  --- per-CMD_WRITE cycles (avg over %lu) ---\n", calls);
        IDOS->Printf("  total   %lu  (%lu us)\n", ctot/calls, (ctot/calls)/100UL);
        IDOS->Printf("  cook    %lu  (%lu us)\n", ccook/calls, (ccook/calls)/100UL);
        IDOS->Printf("  deliver %lu  (%lu us)\n", cdlv/calls, (cdlv/calls)/100UL);
    }

    IExec->CloseDevice((struct IORequest *)r);
    IExec->FreeSysObject(ASOT_IOREQUEST, r);
    IExec->FreeSysObject(ASOT_PORT, p);
    return 0;
}
