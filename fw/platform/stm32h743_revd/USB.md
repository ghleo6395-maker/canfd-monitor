# Rev D USB CDC + MSC

This module integrates the vendored **TinyUSB 0.21.0** stack with STM32H743VIT6
USB2 OTG FS, PA11/PA12 AF10 and the internal full speed PHY. It is an independent
first implementation, not an existing gs_usb/GVRET/SavvyCAN-compatible adapter.
The parser, event bus, TX editor, ring reader and candump formatter reuse the
existing project modules. TinyUSB provides enumeration, CDC ACM and MSC BOT/SCSI.
Its original MIT license remains in `fw/vendor/tinyusb/LICENSE`.

## Integration

Call `usb_device_init()` after HAL/clock setup and `app_init()`, then
`usb_device_task()` once every superloop iteration. The TinyUSB task uses a zero
timeout. `OTG_FS_IRQHandler()` calls only `tud_int_handler(0)`; application events,
filesystem access and protocol parsing run in the foreground. USB owns its IRQ:
do not link STM32's HAL PCD USB middleware or add a second OTG FS handler.

Required TinyUSB source files:

```
src/tusb.c
src/common/tusb_fifo.c
src/device/usbd.c
src/class/cdc/cdc_device.c
src/class/msc/msc_device.c
src/portable/synopsys/dwc2/dcd_dwc2.c
src/portable/synopsys/dwc2/dwc2_common.c
```

Include `fw/vendor/tinyusb/src`, this platform directory, the project `fw`
directory, and the STM32 HAL/CMSIS includes. `tusb_time_millis_api()` uses
`HAL_GetTick()`. Root board setup selects HSI48 for USB and **USB2 SOF** for CRS.
The USB module enables its GPIO/peripheral clock and USB voltage detector.

The composite descriptor has one CDC function (interfaces 0/1; EP81 notification,
EP02 OUT/82 IN), and one MSC function (interface 2; EP03 OUT/83 IN). Bulk maximum
packet size is 64 bytes. CDC RX/TX FIFOs are each 512 bytes, CDC endpoint buffers
64 bytes and MSC endpoint buffer 512 bytes. The application's output ring is
8192 bytes, input line is at most 255 bytes and its MSC bounce buffer is 512 bytes,
aligned to 32. The USB driver uses DWC2 FIFO slave mode with USB DMA disabled.
The SD driver copies data through its own polling buffer; it must never DMA
TinyUSB's buffer. There is no whole-volume RAM disk or framebuffer here.

Default VID `0xCAFD` / PID `0x0001` are **unallocated local development values**.
They do not belong to a certified product and must be replaced with the
maintainer's assigned VID/PID using `REVD_USB_VID` / `REVD_USB_PID` before
hardware distribution. The descriptor declares 500 mA, consistent with Rev D's
USB500 charger limit. The charger cannot switch to USB100 before enumeration,
and this firmware does not establish USB power-compliance certification.

## CDC protocol version 1

Open the CDC ACM port with DTR enabled. Baud-rate requests do not change USB
transport throughput. Commands are case-sensitive ASCII, terminated by LF, CR or
CRLF. Replies begin `OK`, `ERR`, or `STATUS`. There is no remote confirmation
command. Apart from `HELP` and `STATUS`, controls require the device to be in
RUNNING state. After reboot clients should reopen the port and query status.

```
HELP
STATUS
STREAM ON
STREAM OFF
REC START
REC STOP
REC MARK
CH 1 MODE LISTEN
CH 1 MODE RESTRICTED
CH 1 MODE NORMAL
CH 1 RATE 500000 2000000
CH 1 FD ON
CH 1 RECOVER
TX SET 1 0 100 123#01020304
TX SET 2 3 0 18FF50E5##1AABB
TX 1 SEND 0
TX 1 CYC 0 ON
TX 1 CYC 0 OFF
TX STOP
MSC ON
MSC OFF
```

Channels are 1..2 and existing TX slots are 0..3. TX SET period is 0 for single
shot, or 10..60000 ms. Its frame token uses candump syntax: three hexadecimal ID
digits for standard, eight for extended, `#DATA` for classic and `##flagsDATA`
for FD. FD flag 1 means BRS; payload sizes must match a CAN FD DLC. `TX SET`
updates the same atomic editor used by the device UI. It does not send a frame.
Editing an active cyclic slot is rejected by that editor.

`OK QUEUED DEVICE_CONFIRM_IF_REQUIRED` means only that the event was queued.
The service may reject the request or ask for confirmation on the physical
screen. `STATUS` returns numeric service states, pending `ask` enum, recorder
counters, CAN configuration and overflow counters. For example `MSC ON`, `REC
STOP`, entering NORMAL and the first transmission retain the existing device
confirmation flow: source is HOST and confirmation bits are clear. CH RATE's
`b` field is the data bitrate, not a confirmation flag. No format command is
exposed.

`STREAM ON` uploads newly received frames as `FRAME ` followed by a normal
candump line. Responses and complete frame lines share one ordered byte stream;
clients must distinguish the prefix. Commands have a 1536-byte output reserve.
When the host stops reading, RX endpoint backpressure is applied and log frames
are dropped with an explicit `stream_dropped` counter. This independent lossy
ring reader never registers as a recorder/lossless reader. USB slowdown cannot
make the CAN producer wait for the host. Application boot_count changes cancel
streaming, clear old input/output and reattach the reader after ring reset.

This protocol has neither COBS/CRC framing nor an authentication layer. CRC is
provided at the USB link level; a binary network transport is a separate future
protocol. Existing PC CAN tools will need a small protocol adapter. No claim is
made that USB FS can upload two continuously saturated CAN FD channels losslessly.

## Storage exclusivity and detach

MSC enumerates from boot but reports **no medium** until storage_sm has approved
the device confirmation, stopped local file users, unmounted FatFs and called
`port_usb_msc(true)`. `storage_sd_media_give()` must succeed before readiness.
Host START/LOAD cannot grant itself access. Raw callbacks check media ownership,
LUN, bounds and partial-sector offsets. They copy at most one 512-byte sector.
Partial writes first read the sector so unrelated bytes remain unchanged.
SYNCHRONIZE CACHE and eject flush the actual card. Queue saturation preserves a
pending eject event for retry. A failed sync keeps the storage driver's MSC
ownership, hiding the LUN and preventing a local mount until recovery.

There is **no VBUS wire** on Rev D. PGOOD is shared with 12 V and is not used as
USB presence evidence. TinyUSB's unmount callback alone cannot detect unplug with
VBUS sensing disabled. The task polls the DWC2 received-SOF frame number in DSTS;
250 ms without a frame-number change, or host suspend, is conservatively treated
as detach. It hides/flushes/reclaims MSC and posts USB_DETACH/HOST_LINK(0).
Polling avoids filling the USB event queue with 1000 SOF events per second during
SD write latency. On resume/renewed SOF it reports a new USB attachment, but **does
not automatically grant MSC again**. The host should eject before intentional
suspend or cable removal: firmware cannot flush data the host has not sent.
Closing CDC (DTR 0) only disconnects the host control link; a valid MSC session
continues. Per the existing project requirement, previously confirmed host cyclic
transmissions continue offline and remain stoppable from the device screen;
`TX STOP` or the device controls stop them. Disconnect does not grant any new
transmit permission. Storage reclaim is a no-op when the local filesystem,
rather than MSC, owns the card, so ordinary CDC unplug cannot unmount a recording. Real enumeration, suspend behavior and unplug timing still require a
Rev D board and a real host test.

## Verification

`fw/tests/test_usb_protocol.c` verifies bounded token parsing, integer/ID/DLC
limits, maximum FD payload, output preservation on errors and absence of remote
confirmation bypasses. `fw/tests/usb/test_usb_device.c` uses small test-only HAL
and transport facades, plus the real parser/formatter/event queue/ring, to test
MSC ownership, read/write bounds, partial writes, sync failure, DTR/suspend,
frame timeout, saturated eject queues, boot reset and stopped-host streaming.
The facades never access actual USB devices or disks. At implementation time,
197 parser checks and the current transport/ownership regression checks passed with ASan/UBSan. The platform source is
also cross-compiled against the **real** TinyUSB/HAL/CMSIS headers to verify API
compatibility; host mocks alone cannot establish that.

Standalone host ownership test, run from `fw/`:

```sh
make build/host/test_usb_device
./build/host/test_usb_device
```

The Makefile compiles the target STATUS branch against native HAL stubs and
keeps the shared frame ring in ordinary native memory. This also verifies the
largest diagnostic counters without requiring a connected USB device.

Primary API/reference sources checked on 2026-10-24:

- [TinyUSB integration guide](https://github.com/hathach/tinyusb/blob/0.21.0/docs/integration.rst)
- [TinyUSB CDC+MSC example](https://github.com/hathach/tinyusb/tree/0.21.0/examples/device/cdc_msc)
- [TinyUSB DWC2 STM32 port](https://github.com/hathach/tinyusb/blob/0.21.0/src/portable/synopsys/dwc2/dwc2_stm32.h)
- [TinyUSB MSC callbacks](https://github.com/hathach/tinyusb/blob/0.21.0/src/class/msc/msc_device.h)
- [TinyUSB device configuration API](https://github.com/hathach/tinyusb/blob/0.21.0/src/device/usbd.h)

## DMA pipeline revision

`STATUS` identifies `firmware: revd-stress-20261024` and adds `performance`: CAN queue depths/high-water/lost lower bound/software losses, SD acceptance/encoding/rejections/DMA faults, and LCD DMA progress. The format remains protocol 1 with additive JSON fields. Accepted and encoded frames are not a durability acknowledgement.

`usb_device_poll_transport()` services TinyUSB with a reentry guard, without running lifecycle, parsing host commands or consuming the log stream. The SD cooperative wait uses this entry; a raw MSC callback cannot recursively enter TinyUSB. Giving the card to MSC now waits for the queued local unmount and exposes NOT_READY until it succeeds. A five-second pending handoff times out with an eject/error event; physical detach cancels it. IRQ priority is 7, below CAN4 and SDMMC5.

## High-load validation controls

```text
BENCH CONFIG <segment_kib> <rotate_ms> <refresh_ms>
BENCH ON
BENCH OFF
BENCH ROTATE
```

KiB is 64..65536; rotation is 0 (off) or 250..3600000 ms; full-screen
redraw requests are 0 (off) or 50..60000 ms. Configure/ON/OFF require an idle
recorder; ON additionally requires writable READY storage, running power and
no playback or pending/periodic TX. ON uses a session-only segment size override,
keeps the screen awake and leaves recording/CAN mode under normal controls.
ROTATE requires BENCH enabled and recording running. Reboot disables BENCH.
The LVGL refresh cadence can coalesce requests; redraw requests are not FPS.

`STATUS` adds random per-MCU-boot `boot_id` (empty on RNG failure), application
`diag_epoch`, HAL `uptime_ms`, recording session/segment/drop/close result,
BENCH request counts, SD busy/write/sync/task maxima and file/command counters.
`rec_result=0` means IDLE after a normal STOP close acknowledgement; it is 1
otherwise. `rec_reason` retains the application message enum. CAN metrics include
HAL FIFO read errors; LCD metrics separate DMA, SPI and UI snapshot faults.
Maxima and queue peaks are gauges. SD `catalog_limited/catalog_omitted` describe
the current mounted catalog; other driver counts are cumulative since MCU boot.
No reset-statistics command exists; compare a baseline and final sample with the
same boot_id/diag_epoch. Never treat accepted/encoded/f_write byte counts as
power-loss persistence evidence. Final SD-file sequence/CRC verification is
required after normal close, sync and safe media handoff.

The exact command sequence, independent witness requirements and verdicts are
in `fw/tools/bench/README.md` and `docs/software/07_高负载压测与上板验收.md`.


## FreeRTOS transport ownership

B identifies `revd-freertos-20261024` and adds `rtos.scheduler=freertos`, pinned
kernel version, per-task static-stack high-water observations, task step wall
time, bridge drops and UI input/snapshot latency. A retains its existing firmware
identity and fields. Both include `performance.lcd.max_service_gap_us`: active
display-task entry gap measured at HAL millisecond resolution and expressed in
microseconds. Values below 1 ms quantize to zero. Do not confuse this with
`max_render_ms` (UI rendering wall time) or ISR response latency.

In B, `usb_device_task` runs only on core for lifecycle, parser, replies and ring
streaming. Its CDC calls use bounded RX/TX bridge FIFOs; only the USB task calls
TinyUSB and its MSC callbacks. Callbacks publish lifecycle messages atomically
with session FIFO reset; overflow closes the session. MSC raw sector requests
are synchronous RPCs to the storage owner; no TinyUSB or callback enters FatFs.
Core/UI commands preserve normal confirmation rules. A UI snapshot carries a
navigation/confirmation generation, and stale touches are ignored before they
can target a newer page or confirmation. Virtual key gestures from an obsolete
view are canceled without a release action.

The same bench CLI supports both. STATUS monitor records one outstanding request
at a time and reports request-to-complete-response RTT; it includes USB and host
scheduling, and is measured independently of UI input queue latency. Use the
A/B environment template and compare command for matched-condition reports.
