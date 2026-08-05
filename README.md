# loopback.device — SANA-II software loopback for AmigaOS 4

Pure-software SANA-II network device. Every `CMD_WRITE` is delivered
back to every opener's queued `CMD_READ` — no PCI, no DMA, no
hardware, no QEMU roundtrip. A diagnostic instrument: bind Roadshow
to it and measure the bsdsocket + Roadshow + SANA-II stack ceiling
in isolation, without hardware I/O or emulation overhead in the
path.

Sibling to
[`virtnet-amigaos4`](https://github.com/geekychris/virtnet-amigaos4)
(virtio-net) and
[`virte1000-amigaos4`](https://github.com/geekychris/virte1000-amigaos4)
(e1000).

## Status

**Working.** OpenDevice succeeds, two-opener TX→RX round-trip
passes byte-for-byte, Roadshow's `AddNetInterface` binds it,
`ping` against the interface address succeeds. Perf test through
the driver reaches **~19 Mbit/s at 1500-byte frames** on QEMU
sam460ex.

## Design

### Data path (`lo_deliver`)

`CMD_WRITE` handler in `src/device.c`:

1. Look up the sender's `LoOpener` by `ios2_BufferManagement` cookie.
2. Cook the payload (invoke sender's `S2_CopyFromBuff` hook, or raw
   copy if none registered).
3. Walk the opener list. For each opener with a queued `CMD_READ`,
   dequeue one, invoke that opener's `S2_CopyToBuff` hook to
   deliver the frame, then `ReplyMsg` to wake the caller.
4. Fall through to the dispatcher's tail `ReplyMsg` to complete
   the `CMD_WRITE` request.

There is no queue between TX and RX — every write is delivered
synchronously inside the unit task's dispatch of that write.
`CMD_READ` requests that arrive before any `CMD_WRITE` are held
on the opener's `read_queue` until a write matches them up.

### Interface identity

Each `OpenDevice` returns a distinct `LoOpener` keyed by the
caller's `ios2_BufferManagement` tag list pointer. Roadshow
naturally hands each interface its own tag list. Tests that
open the device twice from the same process must supply
**distinct** tag list arrays — reusing the same pointer collides
in `vn_find_opener` and causes `CMD_READ`s from opener B to be
queued to opener A.

### Skeleton origin

The `.device` boilerplate (resident tag, `DeviceManagerInterface`
vector table, `Init`/`Open`/`Close`/`Expunge`/`BeginIO`/`AbortIO`,
per-opener list, unit task with `begin_port`) is byte-copied from
[virtnet-amigaos4](https://github.com/geekychris/virtnet-amigaos4)
and modified in exactly two places:

- `_manager_Init` skips PCI enumeration (loopback owns no
  hardware, and grabbing the virtio-net PCI device would conflict
  with `virtnet.device`).
- The `CMD_WRITE`/`S2_BROADCAST` handler is replaced with
  `lo_deliver`.

`src/virtio.c` and `include/virtio.h` are carried along
unchanged from virtnet — they contain wrappers referenced by
the surviving dispatch code (mostly dead paths never reached
now that Init returns early). Cleanup for later.

### What loopback is NOT

- Not `lo0` / 127.0.0.1 — that's Roadshow's built-in IP-layer
  loopback which bypasses SANA-II entirely. This device is a
  SANA-II driver, so Roadshow talks to it exactly like it talks
  to `virtnet.device` or `virte1000.device`.
- Not a bridge — packets stay inside the driver, never routed.
- Not currently multi-unit — unit 0 only.

## Build

Cross-compile via the `walkero/amigagccondocker` toolchain that
sibling projects use.

```sh
docker pull walkero/amigagccondocker:os4-gcc11-arm64   # once
./scripts/build.sh
```

Artifacts in `build/`:
- `loopback.device` — stripped, ~23 KB, deploy this
- `loopback.device.debug` — with symbols, for `objdump` / GDB
- `testopen`, `testloop`, `testperf`, `teststat`, `testhello`,
  `testopen_vt` — user-space test binaries

## Install

Two paths on the OS4 guest.

### Test-only (recommended during development)

Push the driver anywhere accessible and open it by full path:

```
copy loopback.device DH1:loopback.device
DH1:testopen                 ; expects "OpenDevice loopback.device 0 -> 0"
```

Any crash kills only the test program, not the guest — the driver
is loaded on-demand by exec's `OpenDevice`.

### Roadshow-bound (system-wide interface)

Install the driver and add a Roadshow interface config:

```
copy loopback.device DEVS:Networks/loopback.device
```

Create `DEVS:NetInterfaces/loopdev` (any name **except** `loopback`
— that collides with Roadshow's built-in `lo0`):

```
DEVICE=DEVS:Networks/loopback.device
UNIT=0
ADDRESS=10.99.99.15
NETMASK=255.255.255.0
MTU=1500
ID=loopdev
IPREQUESTS=32
WRITEREQUESTS=32
ARPREQUESTS=32
HARDWAREADDRESS=02:4c:42:00:00:01
```

Bring it up:

```
AddNetInterface loopdev
ShowNetStatus INTERFACES     ; loopdev should be "Up"
ping 10.99.99.15             ; works (via Roadshow IP short-circuit)
```

To have it bind at every boot, add the standard Roadshow autostart
entry (identical pattern to any other Ethernet interface).

## Test

All tests live in `tests/` and are built by `./scripts/build.sh`.

- **`testopen`** — smoke test: opens `loopback.device` unit 0,
  closes it. Should print `OpenDevice loopback.device 0 -> 0`.
- **`testloop`** — the real driver-level verification: two openers
  with distinct `BufferManagement`, opener B queues a `CMD_READ`,
  opener A sends `CMD_WRITE` with a 60-byte pattern, verifies
  byte-for-byte match at B.
- **`testperf [payload_bytes [iterations]]`** — throughput
  measurement. Two openers, pool of pre-queued `CMD_READ`s on B,
  tight loop of `DoIO(CMD_WRITE)` from A. Uses PPC time-base
  register (`mftb`) at 100 MHz for wall-clock. Prints pkts/sec,
  Mbit/s, µs/pkt. Defaults: 1500 bytes, 10000 iterations.
- **`testopen_vt`** — byte-identical to `testopen` but opens
  `virtnet.device` instead. Sanity check that the test binary
  shape works when the driver-under-test is broken.

Typical session on the OS4 guest:

```
DH1:testopen                            ; expects success
DH1:testloop                            ; expects "payload match: YES"
DH1:testperf 1500 10000                 ; ~19 Mbit/s on QEMU sam460ex
DH1:testperf 64   10000                 ; per-packet overhead dominates
```

## Perf numbers (reference)

QEMU sam460ex on Apple M1, PPC 460 emulation, cold cache
between runs:

| Payload | pkts/sec | Mbit/s | µs/pkt |
|---------|----------|--------|--------|
| 1500 B  | 1595     | 19.15  | 626    |
| 64 B    | 1442     | 0.74   | 693    |

Per-packet overhead is essentially independent of payload size —
the copy is free, the cost is the async round-trip through the
unit task's `begin_port` (2 task switches + 2 semaphore ops + 2
copy-hook invocations + 4 message-passing operations per packet).

Native PPC 460 hardware would be dramatically faster; this number
is a QEMU emulation ceiling, not a fundamental SANA-II limit.

## Known limitations

- **Dormant virtio code** — Init returns early before touching PCI,
  but ~2000 lines of virtio init/queue/ISR code are still in the
  binary and never executed. Should be stripped for a clean
  release; harmless in the meantime.
- **Ping-to-self via Roadshow short-circuits SANA-II** — `ping
  10.99.99.15` from the same host succeeds but never touches the
  driver (Roadshow's IP layer routes locally-configured addresses
  without going through the SANA-II layer). To exercise the full
  Roadshow + SANA-II path from user space you need traffic Roadshow
  *can't* short-circuit — e.g. two loopback interfaces on
  different subnets with a static route between them. `testperf`
  bypasses Roadshow entirely and measures the driver ceiling
  directly.
- **Unit 0 only.** Multi-unit is a straightforward extension but
  not implemented.
- **Sender-excluded delivery** — `lo_deliver` currently does
  `if (rx_op == tx_op) continue;` before delivering. This makes
  the two-opener test work naturally but means a *single-opener*
  loopback (send-to-self on the same OpenDevice handle) wouldn't
  receive its own frames. Real Linux `lo0` semantics include self;
  change is a one-liner if needed.

## Repo layout

```
include/loopback.h        struct LoopbackBase (virtnet-derived)
include/version.h         DEVNAME / DEVVER / DEVREV
include/virtio.h          dormant — carried from virtnet
src/device.c              resident tag + Init + Open/Close/Expunge +
                          BeginIO + dispatch + lo_deliver
src/virtio.c              dormant — carried from virtnet
tests/testopen.c          smoke-test OpenDevice
tests/testopen_vt.c       same, but against virtnet.device
tests/testloop.c          two-opener TX→RX byte-match test
tests/testperf.c          driver-level throughput benchmark
tests/teststat.c          reads legacy LO_DBG_STATUS counters
                          (stale — pre-bisect skeleton opcode)
tests/testhello.c         Printf sanity check
scripts/build.sh          docker-run of the cross-compile Makefile
Makefile                  invoked inside the docker container
```

## History

The from-scratch skeleton at commits `820a38f` .. `6d072d4`
crashed inside exec's own `OpenDevice` dispatch (DSI with
stack-guard hit, upstream of the driver's `Open` handler; root
cause never pinned down). The current implementation replaces
that skeleton with a byte-copy of the known-working virtnet
driver, then modifies only the Init (strip PCI) and packet-path
handler. Full history in `git log`.
