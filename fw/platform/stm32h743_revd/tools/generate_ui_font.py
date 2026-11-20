#!/usr/bin/env python3
"""Rebuild the OFL Rev D interface font using LVGL's bundled open font.

Generation requires Node/npm; firmware builds use the checked-in generated C.
No downloaded font or OS font is needed. Keep custom name (OFL reserved names).
"""
import pathlib
import re
import shutil
import subprocess

board = pathlib.Path(__file__).resolve().parents[1]
fw = board.parents[1]
font = fw / 'vendor/lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf'
sources = [fw / 'app/ui/core/ui_sm.c', board / 'ui_lvgl.c']
sources.extend(sorted((fw / 'services').rglob('*.c')))
# Shared device keyboards/send plans still retain their historical sim/ paths.
sources.extend(fw / 'sim' / name for name in
               ('local_files_ui.c','send_plan.c','tx_sequence.c','tx_scope.c'))
chars = set()
# File/project names are user data. Include the 6,763 GB2312 Han characters so
# ordinary Chinese names copied from the PC do not turn into missing glyphs.
# This is generated from Python's built-in mapping and the bundled OFL font;
# it needs neither a downloaded font nor a run-time glyph allocator.
for lead in range(0xB0, 0xF8):
    for trail in range(0xA1, 0xFF):
        try:
            character = bytes((lead, trail)).decode('gb2312')
        except UnicodeDecodeError:
            continue
        if '\u4e00' <= character <= '\u9fff':
            chars.add(character)
for source in sources:
    code = source.read_text()
    code = re.sub(r'/\*.*?\*/|//[^\n]*', '', code, flags=re.S)
    for literal in re.findall(r'"((?:\\.|[^"\\])*)"', code):
        chars.update(c for c in literal if ord(c) >= 128)
symbols = ''.join(sorted(chars, key=ord))
(board / 'fonts/ui_font_chars.txt').write_text(symbols + '\n')
subprocess.run(['npx', '--yes', 'lv_font_conv@1.5.3', '--size', '16',
                '--bpp', '4', '--font', str(font), '--range', '0x20-0x7e',
                '--symbols', symbols, '--format', 'lvgl', '--no-compress',
                '--no-prefilter', '--no-kerning', '--lv-include', 'lvgl.h',
                '--lv-font-name', 'ui_font_revd_16',
                '--output', str(board / 'ui_font_revd_16.c')], check=True)
out = board / 'ui_font_revd_16.c'
s = out.read_text()
s = '/* Derived from Adobe open font; SIL OFL 1.1, see fonts/LICENSE.SourceHanSansSC.txt. */\n' + s
out.write_text(s)
shutil.copyfile(font.parent / 'font_license/SourceHanSansSC/LICENSE.txt',
                board / 'fonts/LICENSE.SourceHanSansSC.txt')
print(f'Generated {len(symbols)} non-ASCII interface glyphs plus printable ASCII')
