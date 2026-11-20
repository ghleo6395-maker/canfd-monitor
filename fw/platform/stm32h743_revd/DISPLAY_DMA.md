# Rev D SPI4 / ST7796S display DMA

The display still uses the pinned LVGL 9.5.0 ST7796 initialization and renderer.
No LVGL vendor source is changed. This target keeps D-cache disabled.

At startup, the stock MIPI command callback sends the short controller setup
commands before recording can start. After setup, a custom public LVGL flush
callback copies RGB565 rows into a CPU-owned 480 × 320 shadow image and calls
`lv_display_flush_ready` immediately **after the copy**. The draw buffer is never
borrowed by DMA; consequently LVGL's single-buffer internal wait does not wait
for SPI completion, including a full-screen refresh with 16 partial areas.

`display_dma_poll(now_ms)` chooses one dirty 20-row band, copies it into a
separate immutable DMA buffer, swaps RGB565 bytes for the panel, and sends the
address commands and pixels as six asynchronous HAL SPI DMA transfers. Each
call starts at most one phase. The `DMA1_Stream0` and `SPI4` IRQ handlers call
ST HAL; application completion/error callbacks only set flags. Main-loop code
advances phases and changes CS/DC after HAL reports SPI EOT. DMA transfer
complete alone is not treated as the end of the SPI wire transaction.

The band dirty bit is cleared before a snapshot is transmitted. A newer LVGL
copy reasserts it while DMA owns the previous copy. Completion does not clear
it, so a screen update arriving during transfer is eventually redrawn. A
round-robin cursor prevents a frequently changing band from starving other
bands. Display updates can coalesce; CAN recording must never depend on each
intermediate visual frame being shown.

The 307,200-byte shadow uses AXI SRAM. The 19,200-byte LVGL draw buffer,
19,200-byte stable DMA buffer, and 32-byte command storage use
`.revd_d2_bss` in DMA1-accessible D2 SRAM. The linker/startup must reserve and
zero this section, and enable the D2 SRAM clocks before using it. Neither DTCM
nor the CPU stack is used as a DMA source. If D-cache is enabled later, cache
maintenance or a correctly configured MPU region is required first.

Each DMA phase has a 100 ms foreground deadline, including the approximately
12.3 ms pixel wire time at the current 12.5 MHz SPI clock. Normal refresh never
calls polling `HAL_SPI_Transmit`, `HAL_SPI_Abort`, a flush wait loop, or a delay.
A fault latches the display inactive, blanks PWM, disables SPI/DMA IRQs,
stops SPI DMA requests, resets only SPI4, deinitializes only DMA1 stream 0,
and clears pending NVIC IRQs. This releases DMA ownership before safe GPIO
poweroff and prevents stale completions from starting another transfer.
Restart requires normal display initialization. Raw runtime rotation/MIPI
commands are intentionally rejected, rather than racing a pixel DMA.

`display_task` handles LVGL/UI/touch in the normal foreground loop;
`display_dma_poll` contains no LVGL, touch, filesystem, or application event
calls. The latter can also be called from the SD IDMA cooperative wait to
advance display transfers without recursive LVGL or FatFs entry.

## Verification

Run from the project root:

```sh
python3 fw/tests/display_dma/run_test.py
```

The test compiles the **production display.c and actual pinned LVGL** with a
hardware HAL double, under address/undefined-behavior sanitizers. It verifies
late DMA completion while real LVGL continues rendering, stable DMA contents,
CS/DC and address phases, RGB565 wire order, same-band updates while in flight,
timeout including tick wrap, DMA start/error faults, pixel-DMA cancellation on
shutdown, ignored late callbacks, and successful reinitialization.

These tests do not validate the physical cable, DMA request routing, SPI signal
integrity, panel timing, or bus load on a real board. A full CPU-rendered UI can
still consume CPU time; DMA removes the SPI wire-time wait, not LVGL rendering
cost. FDCAN IRQ buffering and recorder scheduling remain necessary. Board
validation should run simultaneous dual-channel CAN FD recording and repeated
screen redraws, checking independent CAN hardware/ingress/recorder drop counters.
