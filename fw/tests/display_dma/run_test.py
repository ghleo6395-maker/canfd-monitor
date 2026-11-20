#!/usr/bin/env python3
"""Compile the production display transport against real LVGL and a HAL double."""
import os
import pathlib
import subprocess
root = pathlib.Path(__file__).resolve().parents[2]
out = root / 'build' / 'test_display_dma'
reve = os.environ.get('DISPLAY_TEST_BOARD') == 'reve'
if reve:
    out = out.with_name('test_reve_display_dma')
out.parent.mkdir(exist_ok=True)
sources = sorted((root / 'vendor' / 'lvgl' / 'src').rglob('*.c'))
cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-DLV_CONF_INCLUDE_SIMPLE', '-I'+str(root / 'tests/display_dma/include'),
       '-I'+str(root), '-I'+str(root/'platform/stm32h743_revd'),
       '-I'+str(root/'vendor/lvgl'), str(root/'tests/display_dma/test_display_dma.c'),
       str(root/'platform/stm32h743_revd/ui_font_revd_16.c'),
       str(root/'platform/stm32h743_revd/can_ingress.c'),
       str(root/'services/frame_ring/frame_ring.c'),
       *map(str, sources), '-lm', '-o', str(out)]
if reve:
    cmd.insert(1, '-DCANFD_REVE_TARGET')
# Avoid printing a several-hundred-source command on compilation failure.
result = subprocess.run(cmd, timeout=120)
if result.returncode:
    raise SystemExit(result.returncode)
subprocess.run([str(out)], check=True, timeout=15)
