# Actual FreeRTOS kernel with deterministic peripheral time

```sh
python3 fw/tests/rtos_host/run_host.py --kind smoke
python3 fw/tests/stress/run_stress.py --report work/bare-model.json
python3 fw/tests/rtos_stress/run_kernel.py --report work/rtos-model.json
python3 fw/tools/bench/revd_bench.py compare --models work/bare-model.json work/rtos-model.json --json work/ab.json --csv work/ab.csv
```

`run_kernel.py` imports the existing `SCENARIOS` unchanged: no copied/relaxed
11-scenario fixture list. It includes `pipeline_stress.c` and changes only its
foreground step seam. The original bare-metal runner retains ASan/UBSan.
Pinned upstream FreeRTOS V11.3.2 GCC/POSIX tasks run on independent pthread
stacks with real kernel task notifications; this host-port test does not link
ASan because the port also owns task-switch signals.

The controller drives the same virtual SD wire/programming, SPI wire and CPU
costs. Core/UI/storage workers use a 1 ms virtual period. A storage wait blocks
only that worker stack. IDMA can wake at its completion event. Card programming
uses a single bounded 200 us short-poll budget, then 1 ms block/poll. All native
kernel switching happens for real; its wall-clock duration is not used as an
STM32 timing measurement. Virtual preemption is represented at explicit wait
boundaries, not every instruction or every real M7 IRQ/tick.

`--wait-policy tick-only` intentionally reproduces the rejected strategy where
every normal programming check blocks a tick. It loses throughput in strict
scenarios and exits nonzero; this is a regression fixture, not the shipping
default. Do not lower the frame rate to make it pass.

Output checks all injected/on-card frames, CRC, sequence, timestamps, exact
segment fences, snapshot ownership, overflow/timeout/full-card fault contracts,
plus actual task progress during SD blocking and `max_fatfs_call_depth<=1`.
Core, UI and synthetic control-marker timing use shared observation points in
both models; no clock/cost change is made for these observations.

Limitations: model peripherals, RAM-only card, immediate/no-cost CAN IRQ model,
test UI instead of full product UI, no physical cache/AXI/FTL/analog/power-loss
measurements. The production RTOS RPC wrapper is not linked here; the separate
`tests/rtos_host/test_storage_owner.c` covers it with the same actual kernel.
Outputs are always MODEL and cannot establish hardware speedup or zero loss
outside the modeled assumptions. See `docs/software/09_FreeRTOS对照版本与验收.md`.
