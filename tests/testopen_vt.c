/* testopen_vt — same as testopen but opens virtnet.device.
 * Confirms our test-binary shape works. */

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

    IDOS->Printf("about to OpenDevice virtnet.device\n");
    IDOS->FFlush(IDOS->Output());
    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)r, 0);
    IDOS->Printf("OpenDevice virtnet.device 0 -> %ld\n", (long)err);
    if (err == 0) IExec->CloseDevice((struct IORequest *)r);
    IExec->FreeSysObject(ASOT_IOREQUEST, r);
    IExec->FreeSysObject(ASOT_PORT, p);
    return err ? 20 : 0;
}
