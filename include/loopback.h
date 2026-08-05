#ifndef LOOPBACK_H
#define LOOPBACK_H

/*
 * loopback.device — pure-software SANA-II loopback interface for
 * AmigaOS 4. Every CMD_WRITE is echoed as an RX on the same
 * interface. No PCI, no hardware, no QEMU-side work. Intended as
 * a diagnostic instrument: run iperf3 client + server on the same
 * guest, bind both to a loopback interface, measure the ceiling of
 * Roadshow + bsdsocket + SANA-II with zero PCI/emulation overhead.
 *
 * Layout mirrors virtnet's per-opener state so the SANA-II dispatch
 * layer is a direct copy — only the packet path changes (no
 * hardware ring, no notify, no DMA). CMD_WRITE→CMD_READ handoff
 * happens synchronously inside the unit task.
 */

#include <exec/devices.h>
#include <exec/interfaces.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <interfaces/utility.h>

#include <proto/exec.h>

/* uint8/16/32 come from exec/types.h transitively via the includes
 * above — DO NOT redeclare here (the SDK's uint32 is `unsigned long`
 * on this target, ours would be `unsigned int` = conflict). */

/* Per-opener state — one node per successful OpenDevice. Owned by
 * the driver, indexed by ios2_BufferManagement (which is the tag
 * list Roadshow passed at Open). */
struct LoOpener {
    struct MinNode      node;
    APTR                bm_cookie;         /* == the tag list */
    struct MinList      read_queue;        /* pending CMD_READ ioreqs */
    ULONG              *sana2_hook_methods;
    struct Hook        *sana2_hook;        /* Roadshow's Sana2Hook */
    APTR                copy_to_buff;      /* legacy tag-list variant */
    ULONG               copy_to_tag;
    APTR                copy_from_buff;
    ULONG               copy_from_tag;
    struct MsgPort     *event_port;        /* for S2_ONEVENT deliveries */
    ULONG               tracked_types_bitmap; /* not used yet */
    /* S2_ONEVENT queued requests */
    struct MinList      event_queue;
};

/* Library base. First field MUST be struct Device. */
struct LoopbackBase {
    struct Device       dev_Base;
    struct ExecIFace   *IExec;
    BPTR                dev_SegList;
    struct SignalSemaphore io_lock;

    struct Library     *UtilityBase;
    struct UtilityIFace *IUtility;

    /* Interface state */
    ULONG               state;   /* LO_STATE_* */
    UBYTE               mac[6];  /* configured MAC (from CONFIGINTERFACE
                                    or a default) */
    BOOL                mac_set;

    /* Opener list — protected by opener_lock. */
    struct SignalSemaphore  opener_lock;
    struct MinList          opener_list;

    /* Unit for exec dispatch. Real struct Unit so OS4 exec can
     * enqueue IORequests on the async-command path. */
    struct Unit         lo_unit;

    /* Unit task processes dispatched IORequests. */
    struct Task        *unit_task;
    ULONG               unit_signal_mask;
    struct MsgPort     *begin_port;      /* BeginIO PutMsg's ioreqs here */
    struct Task        *parent_task;
    ULONG               parent_ready_mask;
    BOOL                task_ready_ok;

    /* Stats */
    volatile ULONG      packets_tx;
    volatile ULONG      packets_rx;
    volatile ULONG      bytes_tx;
    volatile ULONG      bytes_rx;
    volatile ULONG      drops_no_reader;   /* CMD_WRITE with no queued
                                              CMD_READ to deliver into */

    /* Per-stage cycle counters (mftb-based). See testprofile in
     * virtnet for the readout format. */
    volatile ULONG      prof_tx_calls;
    volatile ULONG      prof_tx_c_total;
    volatile ULONG      prof_tx_c_cook;
    volatile ULONG      prof_tx_c_deliver;
};

/* State machine — mirrors virtnet's. */
#define LO_STATE_OFFLINE     0
#define LO_STATE_CONFIGURED  1
#define LO_STATE_ONLINE      2

/* Diagnostic commands. Chosen not to collide with SANA-II standard
 * commands (0x0000-0x7FFF driver-specific range per Rev 4). */
#define LO_DBG_STATUS   0xF001   /* fills ios2_Data with 40 bytes of BE ULONGs:
                                    [0] state
                                    [1] packets_tx
                                    [2] packets_rx
                                    [3] bytes_tx
                                    [4] bytes_rx
                                    [5] drops_no_reader
                                    [6] prof_tx_calls
                                    [7] prof_tx_c_total
                                    [8] prof_tx_c_cook
                                    [9] prof_tx_c_deliver  */

#endif
