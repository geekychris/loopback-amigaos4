/* testopen — smoke-test OpenDevice("loopback.device", 0). */
#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>

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

    IDOS->Printf("about to OpenDevice loopback.device\n");
    IDOS->FFlush(IDOS->Output());
    LONG err = IExec->OpenDevice("loopback.device", 0, (struct IORequest *)r, 0);
    IDOS->Printf("OpenDevice loopback.device 0 -> %ld\n", (long)err);
    IDOS->FFlush(IDOS->Output());
    if (err == 0) {
        IExec->CloseDevice((struct IORequest *)r);
    }
    IExec->FreeSysObject(ASOT_IOREQUEST, r);
    IExec->FreeSysObject(ASOT_PORT, p);
    return err ? 20 : 0;
}
