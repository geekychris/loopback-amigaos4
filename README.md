# loopback.device — SANA-II software loopback for AmigaOS 4

Pure-software SANA-II loopback interface. Every CMD_WRITE is
delivered as an RX on the same interface. Intended as a **diagnostic
instrument**: bind Roadshow to it, run an in-guest iperf3 client +
server over the loopback, and measure the ceiling of Roadshow +
bsdsocket + SANA-II with **zero PCI / DMA / QEMU roundtrip**.

If loopback hits N Mbit/s and `virtnet.device` (same host, same
Roadshow) hits M Mbit/s, then N-M is the cost we're paying for the
virtio/QEMU path. If N ≈ M we know the ceiling isn't the emulation.

## Status

**Work in progress — hangs on OpenDevice / gurus on load.**

The device loads cleanly (Init log confirms `state=OFFLINE`, unit
task started, MAC set), but `OpenDevice("loopback.device", 0, ...)`
from a test program hangs and eventually gurus. Our diagnostic
marker (a file written on the first line of `_manager_Open`) never
appears, so the crash is BEFORE our Open handler runs — likely a
resident-tag / interface-vector layout that OS4 exec doesn't like.

The `.data` byte-layout of the resident tag matches `virtnet.device`
(which loads cleanly) verbatim. The difference must be somewhere
else — maybe struct field alignment, the vector table order, or
CLT_* tag ordering.

## Design (once loading works)

- **CMD_WRITE**: build ethernet header from ios2 fields, pull payload
  via `CopyFromBuff` hook into a stack buffer, walk opener list,
  find any queued CMD_READ, deliver via `CopyToBuff`, reply both.
  No async, no queue, no hardware.
- **CMD_READ**: enqueue on opener's read_queue. Delivered by the
  next CMD_WRITE that finds it there.
- **MAC**: default `02:4C:42:00:00:01` (locally-administered, 'LB')
  or override via `S2_CONFIGINTERFACE`.
- **Two openers pattern**: iperf3 client opens one, server opens
  another. Both see traffic delivered to each other's read queues.
  MAC doesn't matter — loopback doesn't filter.

## Build

Uses the same `walkero/amigagccondocker:os4-gcc11-arm64` container
as sibling projects `virtnet-amigaos4` and `virte1000`.

```sh
./scripts/build.sh          # builds build/loopback.device + tests
```

## Repo layout

```
include/loopback.h        struct LoopbackBase, LO_STATE_*, LO_DBG_STATUS
include/version.h         DEVNAME / DEVVER
src/device.c              resident tag + Init/Open/Close/Expunge +
                          BeginIO + dispatch + lo_deliver()
tests/testopen.c          smoke-test OpenDevice
tests/teststat.c          read LO_DBG_STATUS counters
tests/testloop.c          two-opener TX→RX byte-match test
tests/testhello.c         sanity-check Printf output capture
```

## Why the boilerplate is copied from virtnet

Every OS4 SANA-II driver needs: resident tag (writable .data),
DeviceManagerInterface vector table, CLT_* init tag list,
CLT_Vector68K for classic-abi callers, `_start` shell stub. This is
150+ lines of exact-shape boilerplate that has to match what
exec's LoadSeg → InitResident does. The `virtnet.device` and
`virte1000` (Bill Borsari) drivers both work; both use the same
skeleton. loopback fork-copies it and only replaces the packet path.

The one place we've diverged from virtnet is the `Sana2DeviceQuery`
handling (loopback advertises MTU=1500, HardwareType=Ethernet, BPS
=1Gbit) and the packet-path itself (`lo_deliver`).

## Related work in the ecosystem

- `virtnet-amigaos4` — virtio-net driver, ~25 Mbit/s over SLIRP
- `virte1000-amigaos4` (Bill Borsari) — e1000 driver, ~40 Mbit/s
- Linux `drivers/net/loopback.c` — the reference we modelled after
  (6-line `loopback_xmit`, hands sk_buff straight to `__netif_rx`)
