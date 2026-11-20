#!/usr/bin/env python3
"""Build/run the production pipeline model. All card bytes are RAM only.

HAL_GetTick never advances time. Scenario inputs and every duration below are
explicit model parameters, not measurements or guarantees for the Rev D PCB.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCENARIOS = [
    dict(name='dual-5000fps-sd-busy-50ms', fps=2500, duration_ms=5000, busy_ms=50),
    dict(name='dual-5000fps-sd-busy-250ms-over-capacity', fps=2500, duration_ms=5000, busy_ms=250, expect_fault=1),
    dict(name='dual-1000fps-sd-busy-250ms', fps=500, duration_ms=5000, busy_ms=250),
    dict(name='dual-800fps-sd-busy-1000ms', fps=400, duration_ms=5000, busy_ms=1000),
    dict(name='dual-5000fps-sd-busy-1000ms-over-capacity', fps=2500, duration_ms=5000, busy_ms=1000, expect_fault=1),
    dict(name='dual-sd-busy-2500ms-timeout', fps=400, duration_ms=5000, busy_ms=2500, expect_fault=1),
    dict(name='dual-256-continuous-segments-lcd', fps=1000, duration_ms=26000, busy_ms=50, rotate_frames=200, segments=256),
    dict(name='dual-near-full-card', fps=500, duration_ms=5000, busy_ms=0, card_full=1, expect_fault=1),
    dict(name='dual-sync-busy-1000ms-lcd', fps=400, duration_ms=5000, busy_ms=1000, busy_target='sync'),
    dict(name='dual-rotation-close-busy-1000ms-lcd', fps=400, duration_ms=5000, busy_ms=1000, busy_target='close', rotate_frames=100, segments=16),
    dict(name='dual-tick-wrap-busy-lcd' , fps=500, duration_ms=5000, busy_ms=250, clock_start_ms=4294965000),
]

def build(binary):
    local = [ROOT/'tests/stress/pipeline_stress.c',
             ROOT/'platform/stm32h743_revd/storage_sd.c',
             ROOT/'platform/stm32h743_revd/send_plan_json.c',
             ROOT/'platform/stm32h743_revd/diskio_sd.c',
             ROOT/'platform/stm32h743_revd/can_fdcan.c',
             ROOT/'platform/stm32h743_revd/can_ingress.c',
             ROOT/'platform/stm32h743_revd/ui_font_revd_16.c',
             ROOT/'services/frame_ring/frame_ring.c',
             ROOT/'services/logfmt/candump.c',ROOT/'services/config/cfg.c',
             ROOT/'services/device/send_plan_validate.c',ROOT/'services/tx_sched/tx_validate.c',
             ROOT/'services/core/jsonw.c',ROOT/'vendor/fatfs/source/ff.c',ROOT/'vendor/fatfs/source/ffunicode.c']
    lvgl = sorted((ROOT/'vendor/lvgl/src').rglob('*.c'))
    binary.parent.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-DLV_CONF_INCLUDE_SIMPLE','-include',str(ROOT/'platform/stm32h743_revd/fatfs_conf/ffconf.h'),
             '-I'+str(ROOT/'tests/stress/include'),'-I'+str(ROOT),
             '-I'+str(ROOT/'platform/stm32h743_revd'),'-I'+str(ROOT/'vendor/fatfs/source'),
             '-I'+str(ROOT/'vendor/lvgl')]
    config = ''.join((ROOT/path).read_text() for path in [
        'tests/stress/include/stm32h7xx_hal.h','platform/stm32h743_revd/lv_conf.h',
        'platform/stm32h743_revd/fatfs_conf/ffconf.h'])
    cache = binary.parent/'objects'/hashlib.sha256((str(flags)+config).encode()).hexdigest()[:16]
    cache.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get('CC','cc')
    def compile_one(source):
        obj=cache/(hashlib.sha256(str(source).encode()).hexdigest()[:24]+'.o')
        # Always rebuild application objects so changed owning headers are seen.
        if source in lvgl and obj.exists() and obj.stat().st_mtime>=source.stat().st_mtime:
            return obj
        extra=['-Dlogfmt_format_line=stress_format_line','-Df_write=stress_f_write','-Df_sync=stress_f_sync','-Df_close=stress_f_close'] if source.name=='storage_sd.c' else []
        result=subprocess.run([compiler,*flags,*extra,'-c',str(source),'-o',str(obj)],timeout=90)
        if result.returncode: raise RuntimeError(f'Compile failed: {source.name}')
        return obj
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            objects=list(pool.map(compile_one,[*local,*lvgl]))
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from None
    result=subprocess.run([compiler,'-fsanitize=address,undefined',*map(str,objects),'-lm','-o',str(binary)],timeout=90)
    if result.returncode: raise SystemExit(result.returncode)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=pathlib.Path,default=ROOT/'build/stress/report.json')
    parser.add_argument('--scenario',help='Run just one named scenario')
    parser.add_argument('--no-build',action='store_true')
    args=parser.parse_args()
    binary=ROOT/'build/stress/pipeline_stress'
    if not args.no_build: build(binary)
    reports=[]
    for spec in SCENARIOS:
        if args.scenario and args.scenario!=spec['name']: continue
        cmd=[str(binary)]
        for key,value in spec.items(): cmd.extend(['--'+key.replace('_','-'),str(value)])
        completed=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
        if completed.stderr: print(completed.stderr,end='',flush=True)
        if not completed.stdout.strip(): raise SystemExit(f"Scenario crashed: {spec['name']} ({completed.returncode})")
        result=json.loads(completed.stdout.strip().splitlines()[-1])
        reports.append(result)
        classification='ZERO LOSS' if result['zero_loss'] else 'EXPECTED FAULT' if result['pass'] else 'FAILED'
        print(f"{spec['name']}: {classification}; injected={sum(result['injected'])}, card={sum(result['on_card'])}, queue peak={result['queue_peak']}, errors={result['dma_errors']}",flush=True)
    if not reports: raise SystemExit('No scenario selected')
    output={'schema':'revd-pipeline-stress-v1','physical_hardware_tested':False,
            'components':['production can_fdcan.c','production can_ingress.c','production frame_ring.c',
                          'production storage_sd.c','production diskio_sd.c','pinned FatFs',
                          'production display.c','pinned LVGL with test UI'],
            'assumptions':{'sd_wire_bytes_per_second':6250000,'sd_transfer_setup_us':30,
                           'normal_sd_program_busy_us':100,'spi_wire_bits_per_second':12500000,
                           'main_loop_cpu_us':40,'cooperative_poll_cpu_us':50,'candump_encode_cpu_us_per_frame':10,
                           'lcd_partial_cpu_us':200,'irq_model':'immediate after each simulated arrival; zero CPU cost',
                           'not_modeled':['real silicon bus arbitration/cache/interrupt jitter',
                                          'complete production UI CPU cost','analog CAN errors','SD controller hidden FTL behavior',
                                          'power-loss persistence']},
            'all_expectations_met':all(r['pass'] for r in reports),'scenarios':reports}
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(output,ensure_ascii=False,indent=2)+'\n')
    print(f'Report: {args.report}',flush=True)
    raise SystemExit(0 if output['all_expectations_met'] else 1)
if __name__=='__main__': main()
