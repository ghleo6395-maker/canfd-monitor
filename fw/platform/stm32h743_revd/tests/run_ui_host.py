#!/usr/bin/env python3
"""Compile/run real LVGL + application on the host; no physical panel required."""
import os
import pathlib
import subprocess
import sys
import concurrent.futures
import shlex
board = pathlib.Path(__file__).resolve().parents[1]
fw = board.parents[1]
build = fw / 'build/ui-host'
build.mkdir(parents=True, exist_ok=True)
# Both runs use the real widgets. The second defines the actual target UI/core
# branch, but uses a named SD/HAL double: this is not a physical panel/card test.
out = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
for target_core in (False, True):
    name = 'test_ui_lvgl_target_core' if target_core else 'test_ui_lvgl'
    cmd = [os.environ.get('HOST_CC', 'cc'), '-std=c11', '-O1', '-g',
           '-DLV_CONF_INCLUDE_SIMPLE', '-I.', '-I' + str(board), '-Ivendor/lvgl',
           '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-function', '-Wno-unused-parameter']
    if target_core:
        cmd.append('-DCANFD_REVD_TARGET')
    cmd.extend(str(p.relative_to(fw)) for p in (fw / 'vendor/lvgl/src').rglob('*.c'))
    cmd.extend(str(p.relative_to(fw)) for p in (fw / 'services').rglob('*.c'))
    cmd.extend(['app/app.c', 'app/ui/core/ui_sm.c', 'sim/env_sim.c', 'sim/sim_api.c',
                'sim/tx_sequence.c', 'sim/tx_scope.c', 'sim/local_files_ui.c', 'sim/send_plan.c', 'sim/file_port_host.c'])
    if target_core:
        cmd.append(str(board / 'tests/file_port_target_double.c'))
    cmd.extend([str(board / 'ui_font_revd_16.c'), str(board / 'tests/test_ui_lvgl.c'),
                '-o', str(build / name)])
    # Cache individual translation units: changing a view/test need not rebuild
    # hundreds of unchanged LVGL files. Compiler depfiles cover included headers.
    flags = cmd[:cmd.index('vendor/lvgl/src/lv_init.c')]
    sources = cmd[len(flags):-2]
    objects = []
    jobs = []
    for source in sources:
        source_path = pathlib.Path(source)
        if not source_path.is_absolute(): source_path = fw / source_path
        rel = source_path.relative_to(fw)
        obj = build / name / rel.with_suffix('.o')
        dep = obj.with_suffix('.d')
        signature = obj.with_suffix('.flags')
        objects.append(str(obj))
        dependencies = [source_path]
        if dep.exists():
            body = dep.read_text().replace('\\\n', ' ')
            for token in shlex.split(body.partition(':')[2]):
                path = pathlib.Path(token)
                if not path.is_absolute(): path = fw / path
                dependencies.append(path)
        compile_flags = list(flags)
        if rel.as_posix() == 'sim/env_sim.c':
            compile_flags += ['-Dport_sd_file_count=ui_test_env_file_count', '-Dport_sd_file_info=ui_test_env_file_info']
        signature_text = repr(compile_flags)
        stale = (not obj.exists() or not dep.exists() or not signature.exists() or signature.read_text() != signature_text
                 or any(not p.exists() or p.stat().st_mtime_ns > obj.stat().st_mtime_ns for p in dependencies))
        if stale:
            obj.parent.mkdir(parents=True, exist_ok=True)
            jobs.append((compile_flags + ['-MMD', '-MF', str(dep), '-c', str(source_path), '-o', str(obj)], signature, signature_text))
    def compile_one(job):
        command, signature, signature_text = job
        subprocess.run(command, cwd=fw, check=True)
        signature.write_text(signature_text)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(compile_one, jobs))
    binary = build / (name + '.bin')
    subprocess.run([flags[0], '-fsanitize=address,undefined', *objects, '-o', str(binary)], cwd=fw, check=True)
    args = [str(binary)]
    if out:
        images = out / 'target-core' if target_core else out
        images.mkdir(parents=True, exist_ok=True)
        args.append(str(images))
    subprocess.run(args, cwd=fw, check=True)

# Verify the actual FT6336 register decoder and GPIO safety through HAL doubles.
touch_bin = build / 'test_touch_ft6336'
subprocess.run([os.environ.get('HOST_CC', 'cc'), '-std=c11', '-g',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
                '-I' + str(board / 'tests/hal_mock'), str(board / 'touch_ft6336.c'),
                str(board / 'tests/test_touch_ft6336.c'), '-o', str(touch_bin)],
               cwd=fw, check=True)
subprocess.run([str(touch_bin)], cwd=fw, check=True)
