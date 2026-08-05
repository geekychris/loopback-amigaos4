/*
 * loopback.device — pure-software SANA-II loopback for AmigaOS 4.
 *
 * Every CMD_WRITE is delivered as an RX on the same interface in
 * the same unit-task iteration. No PCI, no MMIO, no DMA, no QEMU
 * roundtrip. Diagnostic instrument: bind Roadshow to this and run
 * an in-guest iperf3 client + server over it to measure the ceiling
 * of Roadshow + bsdsocket + SANA-II with zero hardware overhead.
 *
 * The resident-tag / manager-interface boilerplate is copied verbatim
 * from `virtnet.device`, which itself follows the OS4 driver skeleton
 * proven by VirtualSCSIDevice and Bill Borsari's virte1000. The
 * loopback-specific logic is confined to `lo_deliver()` and the
 * dispatch switch in `lo_dispatch_ioreq()`.
 */

#include "loopback.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/errors.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/execbase.h>
#include <devices/newstyle.h>
#include <interfaces/dos.h>

#include <stdarg.h>
#include <stddef.h>

/* ---------- Debug logging (RAM: file) ---------- */

#define LOOPBACK_STATUS_FILE  "RAM:loopback-init.log"

struct LoLogCtx {
    struct Library     *DOSBase;
    struct DOSIFace    *IDOS;
    BPTR                fh;
};

static void lo_log_open(struct ExecIFace *IExec, struct LoLogCtx *ctx)
{
    ctx->DOSBase = NULL; ctx->IDOS = NULL; ctx->fh = (BPTR)NULL;
    ctx->DOSBase = IExec->OpenLibrary("dos.library", 51);
    if (!ctx->DOSBase) return;
    ctx->IDOS = (struct DOSIFace *)IExec->GetInterface(ctx->DOSBase, "main", 1, NULL);
    if (!ctx->IDOS) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; return; }
    ctx->fh = ctx->IDOS->Open((CONST_STRPTR)LOOPBACK_STATUS_FILE, MODE_NEWFILE);
}

static void lo_log_close(struct ExecIFace *IExec, struct LoLogCtx *ctx)
{
    if (ctx->fh) { ctx->IDOS->Close(ctx->fh); ctx->fh = (BPTR)NULL; }
    if (ctx->IDOS) { IExec->DropInterface((struct Interface *)ctx->IDOS); ctx->IDOS = NULL; }
    if (ctx->DOSBase) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; }
}

#define LOGF(ctx, ...) \
    do { if ((ctx).fh) (ctx).IDOS->FPrintf((ctx).fh, __VA_ARGS__); } while (0)

/* ---------- PPC time-base helper for profiling ---------- */

static inline uint32 lo_tb(void)
{
    uint32 t;
    __asm__ volatile ("mftb %0" : "=r"(t) : : "memory");
    return t;
}

/* ---------- Standard NSD supported-command list ---------- */

static const UWORD lo_supported_cmds[] = {
    NSCMD_DEVICEQUERY,
    CMD_READ,
    CMD_WRITE,
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ONLINE,
    S2_OFFLINE,
    S2_BROADCAST,
    S2_READORPHAN,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,
    S2_TRACKTYPE,
    S2_UNTRACKTYPE,
    S2_SANA2HOOK,
    LO_DBG_STATUS,
    0
};

/* ---------- Manager Obtain/Release ---------- */

uint32 _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

uint32 _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* ---------- Forward decls for vectors ---------- */

extern struct Library *_manager_Init(struct Library *lib, BPTR seglist, struct Interface *exec);
extern struct LoopbackBase *_manager_Open(struct DeviceManagerInterface *Self,
                                            struct IOSana2Req *ioreq,
                                            ULONG unitNum, ULONG flags);
extern BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
extern void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);

static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL, (APTR)NULL,
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name,        (ULONG)"__device"},
    {MIT_VectorTable, (ULONG)_manager_Vectors},
    {MIT_Version,     1},
    {TAG_END,         0},
};

const APTR devInterfaces[] = { (APTR)_manager_Tags, (APTR)NULL };

/* 68k-compat jump table — same order as classic devices. */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,     /* -6  */
    (APTR)_manager_Close,    /* -12 */
    (APTR)_manager_Expunge,  /* -18 */
    (APTR)NULL,              /* -24 Reserved */
    (APTR)_manager_BeginIO,  /* -30 */
    (APTR)_manager_AbortIO,  /* -36 */
    (APTR)-1,
};

static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct LoopbackBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

static struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    DEVMAJOR,
    NT_DEVICE,
    0,
    (STRPTR)DEVNAME,
    (STRPTR)DEVVERSIONSTRING,
    (APTR)dev_init_tags,
};

int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to DEVS:Networks/ or "
                       "OpenDevice() from a test program.\n", DEVNAME);
    return 20;
}

/* ---------- Opener list + copy hook helpers ---------- */

static struct LoOpener *lo_find_opener(struct LoopbackBase *base, APTR bm_cookie)
{
    struct LoOpener *o;
    for (o = (struct LoOpener *)base->opener_list.mlh_Head;
         o->node.mln_Succ;
         o = (struct LoOpener *)o->node.mln_Succ) {
        if (o->bm_cookie == bm_cookie) return o;
    }
    return NULL;
}

static BOOL lo_invoke_copy_from(struct LoopbackBase *base,
                                 struct LoOpener *op,
                                 struct IOSana2Req *ioreq,
                                 APTR to, ULONG size)
{
    if (!op || !base->IUtility) return FALSE;
    struct Hook *hook = op->sana2_hook ? op->sana2_hook : (struct Hook *)op->copy_from_buff;
    if (!hook) return FALSE;
    ULONG tag = op->sana2_hook ? S2_CopyFromBuff : op->copy_from_tag;
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = to;
    msg.schm_From    = ioreq->ios2_Data;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(hook, ioreq, &msg);
}

static BOOL lo_invoke_copy_to(struct LoopbackBase *base,
                               struct LoOpener *op,
                               struct IOSana2Req *ioreq,
                               APTR from, ULONG size)
{
    if (!op || !base->IUtility) return FALSE;
    struct Hook *hook = op->sana2_hook ? op->sana2_hook : (struct Hook *)op->copy_to_buff;
    if (!hook) return FALSE;
    ULONG tag = op->sana2_hook ? S2_CopyToBuff : op->copy_to_tag;
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = ioreq->ios2_Data;
    msg.schm_From    = from;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(hook, ioreq, &msg);
}

/* lo_find_opener is currently unused (dispatch identifies opener via
 * ioreq->ios2_BufferManagement which we replaced with our own ptr in
 * _manager_Open). Keeping declared for future MAC-filter support. */
__attribute__((used)) static struct LoOpener *lo_find_opener_ref(struct LoopbackBase *b, APTR c) { return lo_find_opener(b, c); }

/* ---------- The core loopback packet path ---------- */

static void lo_deliver(struct LoopbackBase *base,
                        struct IOSana2Req *tx_req,
                        struct LoOpener *tx_op)
{
    struct ExecIFace *IExec = base->IExec;
    UBYTE frame[1600];

    ULONG plen = tx_req->ios2_DataLength;
    if (plen > 1514) plen = 1514;

    for (int i = 0; i < 6; i++) frame[i]     = tx_req->ios2_DstAddr[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = base->mac[i];
    uint16 etype = (uint16)(tx_req->ios2_PacketType & 0xFFFF);
    frame[12] = (UBYTE)(etype >> 8);
    frame[13] = (UBYTE)(etype & 0xFF);

    BOOL cooked = (tx_op != NULL) && (base->IUtility != NULL) &&
                  (tx_op->sana2_hook != NULL || tx_op->copy_from_buff != NULL);

    ULONG frame_len;
    uint32 _c0 = lo_tb();
    if (cooked) {
        BOOL ok = lo_invoke_copy_from(base, tx_op, tx_req, frame + 14, plen);
        if (!ok) {
            tx_req->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            tx_req->ios2_WireError    = S2WERR_BUFF_ERROR;
            return;
        }
        frame_len = 14 + plen;
    } else {
        UBYTE *src = (UBYTE *)tx_req->ios2_Data;
        if (plen > sizeof(frame)) plen = sizeof(frame);
        for (ULONG i = 0; i < plen; i++) frame[i] = src[i];
        frame_len = plen;
    }
    base->prof_tx_c_cook += (lo_tb() - _c0);

    uint32 _d0 = lo_tb();
    UBYTE *pay      = frame + 14;
    ULONG  pay_len  = frame_len - 14;
    BOOL   delivered = FALSE;
    IExec->ObtainSemaphore(&base->opener_lock);
    for (struct LoOpener *o = (struct LoOpener *)base->opener_list.mlh_Head;
         o->node.mln_Succ;
         o = (struct LoOpener *)o->node.mln_Succ) {
        if (!o->read_queue.mlh_Head || !o->read_queue.mlh_Head->mln_Succ) continue;
        struct IOSana2Req *rx_req = (struct IOSana2Req *)IExec->RemHead(
            (struct List *)&o->read_queue);
        IExec->ReleaseSemaphore(&base->opener_lock);

        for (int i = 0; i < 6; i++) rx_req->ios2_SrcAddr[i] = frame[6 + i];
        for (int i = 0; i < 6; i++) rx_req->ios2_DstAddr[i] = frame[i];
        rx_req->ios2_PacketType = ((ULONG)frame[12] << 8) | (ULONG)frame[13];
        rx_req->ios2_DataLength = pay_len;

        BOOL ok = lo_invoke_copy_to(base, o, rx_req, pay, pay_len);
        rx_req->ios2_Req.io_Error = ok ? 0 : S2ERR_NO_RESOURCES;
        IExec->ReplyMsg((struct Message *)rx_req);
        if (ok) {
            delivered = TRUE;
            base->packets_rx++;
            base->bytes_rx += pay_len;
        }
        IExec->ObtainSemaphore(&base->opener_lock);
        break;
    }
    IExec->ReleaseSemaphore(&base->opener_lock);
    base->prof_tx_c_deliver += (lo_tb() - _d0);

    if (!delivered) base->drops_no_reader++;
}

/* ---------- Unit task ---------- */

static void lo_dispatch_ioreq(struct LoopbackBase *base, struct IOSana2Req *ioreq);

static void lo_task_body(void)
{
    struct ExecIFace *IExec = ((struct ExecIFace *)
        ((*(struct ExecBase **)4)->MainInterface));
    struct Task *self = IExec->FindTask(NULL);
    struct LoopbackBase *base = (struct LoopbackBase *)self->tc_UserData;

    base->begin_port = (struct MsgPort *)IExec->AllocSysObjectTags(
        ASOT_PORT, ASOPORT_AllocSig, TRUE, TAG_END);
    if (!base->begin_port) {
        base->task_ready_ok = FALSE;
        IExec->Signal(base->parent_task, base->parent_ready_mask);
        return;
    }
    base->unit_signal_mask = 1UL << base->begin_port->mp_SigBit;

    base->task_ready_ok = TRUE;
    IExec->Signal(base->parent_task, base->parent_ready_mask);

    for (;;) {
        ULONG sigs = IExec->Wait(base->unit_signal_mask | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C) break;
        struct IOSana2Req *ioreq;
        while ((ioreq = (struct IOSana2Req *)IExec->GetMsg(base->begin_port))) {
            lo_dispatch_ioreq(base, ioreq);
        }
    }
}

static BOOL lo_task_start(struct LoopbackBase *base, struct ExecIFace *iexec)
{
    LONG ready_bit = iexec->AllocSignal(-1);
    if (ready_bit < 0) return FALSE;

    base->task_ready_ok      = FALSE;
    base->parent_task        = iexec->FindTask(NULL);
    base->parent_ready_mask  = 1UL << ready_bit;

    iexec->Forbid();
    struct Task *t = iexec->CreateTaskTags("loopback-unit", 20,
                                           lo_task_body, 8192,
                                           TAG_END);
    if (t) t->tc_UserData = (APTR)base;
    base->unit_task = t;
    iexec->Permit();

    if (!t) { iexec->FreeSignal(ready_bit); return FALSE; }
    (void)iexec->Wait(base->parent_ready_mask);
    iexec->FreeSignal(ready_bit);
    return base->task_ready_ok;
}

/* ---------- Init / Open / Close / Expunge / BeginIO / AbortIO ---------- */

struct Library *_manager_Init(struct Library *lib, BPTR seglist, struct Interface *exec)
{
    struct ExecIFace *iexec = (struct ExecIFace *)exec;
    struct LoopbackBase *base = (struct LoopbackBase *)lib;
    base->IExec       = iexec;
    base->dev_SegList = seglist;
    base->state       = LO_STATE_OFFLINE;

    struct LoLogCtx log = {0};
    lo_log_open(iexec, &log);
    LOGF(log, (CONST_STRPTR)"=== Init %s %ld.%ld (%s) ===\n",
         (CONST_STRPTR)DEVNAME, (LONG)DEVMAJOR, (LONG)DEVMINOR,
         (CONST_STRPTR)__DATE__);

    /* Explicitly fill dd_Library fields — matches virtnet.device
     * which works. LIBF_SUMUSED | LIBF_CHANGED is what virtnet uses;
     * exec accepts these on OS4 (LIBF_CHANGED just means "please
     * recompute checksum next time"; SumLibrary is a stub for
     * modern-interface devices). */
    base->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    base->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    base->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    base->dev_Base.dd_Library.lib_Version      = DEVMAJOR;
    base->dev_Base.dd_Library.lib_Revision     = DEVREVISION;
    base->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    iexec->InitSemaphore(&base->io_lock);
    iexec->InitSemaphore(&base->opener_lock);
    iexec->NewList((struct List *)&base->opener_list);

    /* Initialize the embedded lo_unit. Exec's async dispatch path
     * (SendIO / DoIO for non-QUICK requests) checks unit_MsgPort;
     * an uninitialized MsgPort with a NULL mp_MsgList head causes
     * PutMsg to walk into random memory and OpenDevice to hang.
     * PA_IGNORE + empty list means "we handle everything in BeginIO
     * ourselves; no signal, no consumer task on the unit port". */
    {
        struct Unit *u = &base->lo_unit;
        u->unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        u->unit_MsgPort.mp_Node.ln_Name = (STRPTR)DEVNAME;
        u->unit_MsgPort.mp_Node.ln_Pri  = 0;
        u->unit_MsgPort.mp_Flags        = PA_IGNORE;
        u->unit_MsgPort.mp_SigBit       = 0;
        u->unit_MsgPort.mp_SigTask      = NULL;
        iexec->NewList(&u->unit_MsgPort.mp_MsgList);
        u->unit_flags   = UNITF_ACTIVE;
        u->unit_pad     = 0;
        u->unit_OpenCnt = 0;
    }

    /* Default MAC — locally-administered range. */
    base->mac[0] = 0x02;
    base->mac[1] = 0x4C; /* 'L' */
    base->mac[2] = 0x42; /* 'B' */
    base->mac[3] = 0x00;
    base->mac[4] = 0x00;
    base->mac[5] = 0x01;

    /* Init tracer — marker for QEMU-monitor grep, and a signal
     * that Init reached this point. */
    base->packets_tx = 0xDEADBEEF;
    base->packets_rx = 0xFEEDFACE;
    base->bytes_tx   = 0xC0FFEE00;
    base->bytes_rx   = (ULONG)base;   /* self-address so we can find base */

    base->UtilityBase = iexec->OpenLibrary("utility.library", 51);
    if (base->UtilityBase) {
        base->IUtility = (struct UtilityIFace *)iexec->GetInterface(
            base->UtilityBase, "main", 1, NULL);
    }
    LOGF(log, (CONST_STRPTR)"utility: %s\n",
         base->IUtility ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED");

    BOOL task_ok = lo_task_start(base, iexec);
    LOGF(log, (CONST_STRPTR)"unit task: %s\n",
         task_ok ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED");
    LOGF(log, (CONST_STRPTR)"Init done: state=OFFLINE mac=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
         (ULONG)base->mac[0], (ULONG)base->mac[1], (ULONG)base->mac[2],
         (ULONG)base->mac[3], (ULONG)base->mac[4], (ULONG)base->mac[5]);
    LOGF(log, (CONST_STRPTR)"base=%p packets_tx=@%p\n",
         base, &base->packets_tx);
    lo_log_close(iexec, &log);

    return (struct Library *)base;
}

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct LoopbackBase *base = (struct LoopbackBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    if (base->dev_Base.dd_Library.lib_OpenCnt) {
        /* Deferred expunge — mark for later. */
        base->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)NULL;
    }

    if (base->unit_task) {
        IExec->Signal(base->unit_task, SIGBREAKF_CTRL_C);
        /* Give the task time to exit — simple wait via TimerDevice
         * would be cleaner but adds a dependency; brief spin is fine
         * because the task is at pri 20 and will preempt us. */
        for (int i = 0; i < 100 && base->unit_task &&
             (base->unit_task->tc_State != TS_INVALID); i++) {
            IExec->Wait(0);   /* yield once per iteration */
        }
    }
    if (base->begin_port) {
        IExec->FreeSysObject(ASOT_PORT, base->begin_port);
        base->begin_port = NULL;
    }
    if (base->IUtility) {
        IExec->DropInterface((struct Interface *)base->IUtility);
        base->IUtility = NULL;
    }
    if (base->UtilityBase) {
        IExec->CloseLibrary(base->UtilityBase);
        base->UtilityBase = NULL;
    }
    return base->dev_SegList;
}

struct LoopbackBase *_manager_Open(struct DeviceManagerInterface *Self,
                                     struct IOSana2Req *ioreq,
                                     ULONG unitNum, ULONG flags)
{
    (void)flags;
    struct LoopbackBase *base = (struct LoopbackBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    /* Tracer: proves Open was entered. Readable via LO_DBG_STATUS
     * (packets_tx slot). Also written from another task via teststat
     * won't clobber this because BeginIO for DBG cmds is fast-path
     * synchronous — no interleaving. */
    base->packets_tx = 0xDEADBEEF;

    IExec->ObtainSemaphore(&base->io_lock);
    base->packets_tx = 0xCAFEBABE;   /* passed ObtainSemaphore */
    base->dev_Base.dd_Library.lib_OpenCnt++;

    if (unitNum != 0) {
        ioreq->ios2_Req.io_Error  = IOERR_OPENFAIL;
        ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
        ioreq->ios2_Req.io_Device = (struct Device *)-1;
        base->dev_Base.dd_Library.lib_OpenCnt--;
        IExec->ReleaseSemaphore(&base->io_lock);
        return NULL;
    }

    ioreq->ios2_Req.io_Unit   = &base->lo_unit;
    ioreq->ios2_Req.io_Device = (struct Device *)base;
    ioreq->ios2_Req.io_Error  = 0;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    base->lo_unit.unit_OpenCnt++;
    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    struct LoOpener *op = (struct LoOpener *)IExec->AllocVecTags(
        sizeof(struct LoOpener),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (op) {
        IExec->NewList((struct List *)&op->read_queue);
        IExec->NewList((struct List *)&op->event_queue);

        struct TagItem *tags = (struct TagItem *)ioreq->ios2_BufferManagement;
        if (tags && base->IUtility) {
            op->copy_to_buff = (APTR)base->IUtility->GetTagData(S2_CopyToBuff32, 0, tags);
            op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff32 : 0;
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)base->IUtility->GetTagData(S2_CopyToBuff16, 0, tags);
                op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff16 : 0;
            }
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)base->IUtility->GetTagData(S2_CopyToBuff, 0, tags);
                op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff : 0;
            }
            op->copy_from_buff = (APTR)base->IUtility->GetTagData(S2_CopyFromBuff32, 0, tags);
            op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff32 : 0;
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)base->IUtility->GetTagData(S2_CopyFromBuff16, 0, tags);
                op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff16 : 0;
            }
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)base->IUtility->GetTagData(S2_CopyFromBuff, 0, tags);
                op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff : 0;
            }
        }
        op->bm_cookie = ioreq->ios2_BufferManagement;

        IExec->ObtainSemaphore(&base->opener_lock);
        IExec->AddTail((struct List *)&base->opener_list, (struct Node *)&op->node);
        IExec->ReleaseSemaphore(&base->opener_lock);

        ioreq->ios2_BufferManagement = (APTR)op;
    }

    IExec->ReleaseSemaphore(&base->io_lock);
    return base;
}

/* Original Open body — disabled while we debug the load-crash. */
BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct LoopbackBase *base = (struct LoopbackBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    IExec->ObtainSemaphore(&base->io_lock);
    struct LoOpener *op = (struct LoOpener *)ioreq->ios2_BufferManagement;
    if (op) {
        IExec->ObtainSemaphore(&base->opener_lock);
        IExec->Remove((struct Node *)&op->node);
        IExec->ReleaseSemaphore(&base->opener_lock);
        IExec->FreeVec(op);
    }
    base->lo_unit.unit_OpenCnt--;
    base->dev_Base.dd_Library.lib_OpenCnt--;
    IExec->ReleaseSemaphore(&base->io_lock);
    return (BPTR)NULL;
}

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct LoopbackBase *base = (struct LoopbackBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    /* Fast path for read-only DBG commands — dispatch synchronously
     * so tests without a task can DoIO them. */
    UWORD cmd = ioreq->ios2_Req.io_Command;
    if (cmd == LO_DBG_STATUS || cmd == S2_DEVICEQUERY ||
        cmd == NSCMD_DEVICEQUERY || cmd == S2_GETSTATIONADDRESS) {
        lo_dispatch_ioreq(base, ioreq);
        return;
    }

    /* Async — send to unit task. If task didn't come up (begin_port
     * NULL), dispatch synchronously so we don't PutMsg(NULL, ...). */
    if (!base->begin_port) {
        lo_dispatch_ioreq(base, ioreq);
        return;
    }
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    IExec->PutMsg(base->begin_port, (struct Message *)ioreq);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    (void)Self;
    return ioreq->ios2_Req.io_Error;
}

/* ---------- Dispatch ---------- */

static void lo_dispatch_ioreq(struct LoopbackBase *base, struct IOSana2Req *ioreq)
{
    struct ExecIFace *IExec = base->IExec;
    UWORD cmd = ioreq->ios2_Req.io_Command;
    struct LoOpener *op = (struct LoOpener *)ioreq->ios2_BufferManagement;
    BOOL will_reply = TRUE;

    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    switch (cmd) {

    case CMD_WRITE:
    case S2_BROADCAST: {
        if (base->state != LO_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        uint32 _t0 = lo_tb();
        base->prof_tx_calls++;
        base->packets_tx++;
        base->bytes_tx += ioreq->ios2_DataLength;
        lo_deliver(base, ioreq, op);
        base->prof_tx_c_total += (lo_tb() - _t0);
        break;
    }

    case CMD_READ:
    case S2_READORPHAN: {
        if (base->state != LO_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        if (!op) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            break;
        }
        IExec->ObtainSemaphore(&base->opener_lock);
        IExec->AddTail((struct List *)&op->read_queue,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->ReleaseSemaphore(&base->opener_lock);
        will_reply = FALSE;   /* held for future TX */
        break;
    }

    case S2_ONLINE:
        base->state = LO_STATE_ONLINE;
        break;

    case S2_OFFLINE:
        base->state = LO_STATE_CONFIGURED;
        break;

    case S2_CONFIGINTERFACE: {
        BOOL any = FALSE;
        for (int i = 0; i < 6; i++) if (ioreq->ios2_SrcAddr[i]) any = TRUE;
        if (any) {
            for (int i = 0; i < 6; i++) base->mac[i] = ioreq->ios2_SrcAddr[i];
        }
        base->mac_set = TRUE;
        base->state   = LO_STATE_CONFIGURED;
        break;
    }

    case S2_GETSTATIONADDRESS:
        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = base->mac[i];
            ioreq->ios2_DstAddr[i] = base->mac[i];
        }
        break;

    case S2_DEVICEQUERY: {
        struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) q = (struct Sana2DeviceQuery *)ioreq->ios2_Data;
        if (!q) { ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES; break; }
        UBYTE *raw = (UBYTE *)q;
        ULONG supply = 34;
        {
            volatile UBYTE *vraw = raw;
            for (ULONG i = 0; i < supply; i++) vraw[i] = 0;
        }
        #define WU16(o,v) do { if (supply>=(o)+2){raw[(o)+0]=(UBYTE)((v)>>8);raw[(o)+1]=(UBYTE)(v);} } while(0)
        #define WU32(o,v) do { if (supply>=(o)+4){raw[(o)+0]=(UBYTE)((v)>>24);raw[(o)+1]=(UBYTE)((v)>>16);raw[(o)+2]=(UBYTE)((v)>>8);raw[(o)+3]=(UBYTE)(v);} } while(0)
        WU32( 0, q->SizeAvailable);
        WU32( 4, supply);
        WU32( 8, 0);
        WU32(12, 0);
        WU16(16, 48);
        WU32(18, 1500);
        WU32(22, 1000000000UL);
        WU32(26, S2WireType_Ethernet);
        WU32(30, 1514);
        #undef WU16
        #undef WU32
        break;
    }

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        break;   /* accept */

    case S2_SANA2HOOK: {
        struct Sana2Hook *hook = (struct Sana2Hook *)ioreq->ios2_Data;
        if (!hook) hook = (struct Sana2Hook *)ioreq->ios2_StatData;
        if (!hook || !op) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            break;
        }
        op->sana2_hook         = &hook->s2h_Hook;
        op->sana2_hook_methods = (ULONG *)hook->s2h_Methods;
        break;
    }

    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *nsdq = (struct NSDeviceQueryResult *)ioreq->ios2_Data;
        if (!nsdq || ioreq->ios2_DataLength < sizeof(*nsdq)) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
            break;
        }
        nsdq->DevQueryFormat = 0;
        nsdq->SizeAvailable  = sizeof(*nsdq);
        nsdq->DeviceType     = NSDEVTYPE_SANA2;
        nsdq->DeviceSubType  = 0;
        nsdq->SupportedCommands = (UWORD *)lo_supported_cmds;
        ioreq->ios2_DataLength = sizeof(*nsdq);
        break;
    }

    case LO_DBG_STATUS: {
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        if (!out || ioreq->ios2_DataLength < 40) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
            break;
        }
        ULONG v[10];
        v[0] = base->state;
        v[1] = base->packets_tx;
        v[2] = base->packets_rx;
        v[3] = base->bytes_tx;
        v[4] = base->bytes_rx;
        v[5] = base->drops_no_reader;
        v[6] = base->prof_tx_calls;
        v[7] = base->prof_tx_c_total;
        v[8] = base->prof_tx_c_cook;
        v[9] = base->prof_tx_c_deliver;
        for (int i = 0; i < 10; i++) {
            out[i*4+0]=(UBYTE)(v[i]>>24);
            out[i*4+1]=(UBYTE)(v[i]>>16);
            out[i*4+2]=(UBYTE)(v[i]>>8);
            out[i*4+3]=(UBYTE)(v[i]);
        }
        ioreq->ios2_DataLength = 40;
        break;
    }

    default:
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    if (will_reply) {
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        IExec->ReplyMsg((struct Message *)ioreq);
    }
}
