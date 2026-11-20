#!/usr/bin/env python3
"""Run the real CANFD_REVD_TARGET display-buffer code on the host."""
from pathlib import Path
import os
import subprocess

fw=Path(__file__).resolve().parents[2]
build=fw/'build/disp-target'
build.mkdir(parents=True,exist_ok=True)
target=build/'test_disp_target'
subprocess.run([os.environ.get('HOST_CC','cc'),'-std=c11','-O1','-g',
                '-Wall','-Wextra','-Werror','-DCANFD_REVD_TARGET','-I.',
                '-fsanitize=address,undefined',
                'tests/disp/test_disp_target.c','services/disp/disp_pipe.c',
                'services/frame_ring/frame_ring.c','services/core/app_timer.c',
                'services/core/app_evt.c','-o',str(target)],cwd=fw,check=True)
subprocess.run([str(target)],cwd=fw,check=True)
