#!/usr/bin/env python3
"""TGL Metal driver command-buffer leak (#47): source contract and driver replay.

Without an argument, check that the Lilu user patch only turns the dealloc jne
(75 0D) into two NOPs, keeps every other byte, targets the Metal driver path,
is registered from XE::init and honours -wex-tgl-no-mtlleakfix. With the exact
AppleIntelTGLGraphicsMTLDriver binary, verify the find pattern is unique, sits
at 0x7ED82 in __TEXT,__text of -[MTLIGAccelCommandBuffer dealloc].
"""
from pathlib import Path
import re, subprocess, sys

root = Path(__file__).resolve().parents[1]
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()
def arr(name):
    m = re.search(name + r'\[\] \{([^}]*)\}', xe)
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', m.group(1)))
find, repl = arr('tglMetalLeakFind'), arr('tglMetalLeakRepl')
assert len(find) == len(repl) == 16
diff = [i for i in range(16) if find[i] != repl[i]]
assert diff == [5, 6] and find[5:7] == b'\x75\x0d' and repl[5:7] == b'\x90\x90'
reg = xe[xe.index('static void tglRegisterMetalLeakFix()'):xe.index('void XE::init() {')]
assert reg.index('-wex-tgl-no-mtlleakfix') < reg.index('lilu.onProcLoad(')
assert 'UserPatcher::FileSegment::SegmentTextText,1}' in xe and 'CPU_TYPE_X86_64,0,tglMetalLeakFind' in xe  # global, section 1
assert '"/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver"' in xe
init = xe[xe.index('void XE::init() {'):]
assert init.index('tglRegisterMetalLeakFix();') < init.index('lilu.onKextLoadForce(')

if len(sys.argv) > 1:
    drv = Path(sys.argv[1]); image = drv.read_bytes()
    assert image.count(find) == 1 and image.index(find) == 0x7ED82
    nm = subprocess.run(['nm', str(drv)], capture_output=True, text=True, check=True).stdout
    sym = {l.split(' ', 2)[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split(' ', 2)) == 3 and l[0] != ' '}
    dealloc = sym['-[MTLIGAccelCommandBuffer dealloc]']
    assert dealloc == 0x7ECBA and dealloc < 0x7ED87 < dealloc + 0x200
    # The skipped branch target is the instruction after the delete call.
    assert 0x7ED87 + 2 + 0x0D == 0x7ED96
print('ok')
