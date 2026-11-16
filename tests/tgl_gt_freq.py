#!/usr/bin/env python3
"""TGL GT frequency floor: source contract and donor replay.

Without an argument, check that only a zero RPNSWREQ request is raised, to
1500 MHz in Gen9+ units (ratio 90 << 23), that the opt-out is honoured and
that the donor entry and store are qualified before routing. With the exact
AppleIntelTGLGraphics binary, verify the routed symbol, its prologue, that it
stores its argument to RPNSWREQ (0xA008), and that submitExecList passes a
zero-initialised power-query result (3A70A..3A74D).
"""
from pathlib import Path
import re, subprocess, sys

root = Path(__file__).resolve().parents[1]
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()
setter = xe[xe.index('void tglGTFreqSet('):xe.index('bool tglGTFreqSerialize(')]
assert 'if (!value && tglGTFreqFloor)' in setter and setter.index('value=tglGTFreqFloor') < setter.index('FunctionCast(tglGTFreqSet,tglGTFreqOriginal)(streamer,value)')
install = xe[xe.index('static void tglInstallGTFreqFloor('):]
install = install[:install.index('\n}\n')]
assert install.index('-wex-tgl-no-gtfreq') < install.index('routeMultiple')
assert install.index('memcmp(reinterpret_cast<void *>(fn),head') < install.index('routeMultiple')
assert install.index('memcmp(reinterpret_cast<void *>(address+0x3BFBF),store') < install.index('routeMultiple')
assert 'const uint32_t ratio=1500*3/50;' in install and 'tglGTFreqFloor=ratio<<23;' in install
assert (1500 * 3 // 50) << 23 == 0x2D000000
assert 'tglInstallGTFreqFloor(patcher,index,address,size);' in xe[xe.index('bool XE::processTGLAccelerator('):][:8000]
head = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', re.search(r'head\[\] \{([^}]*)\}', install).group(1)))
store = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', re.search(r'store\[\] \{([^}]*)\}', install).group(1)))

if len(sys.argv) > 1:
    donor = Path(sys.argv[1]); image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    fn = sym['__ZN26IGHardwareCommandStreamer518setGTFrequencyMMIOEj']
    assert fn == 0x3BF6E and image[fn:fn + len(head)] == head
    assert image[0x3BFBF:0x3BFBF + len(store)] == store
    # submitExecList: xorl r15d,r15d ... movl r15d,(rcx) (zero default) ... movl r15d,esi; call setGTFrequencyMMIO
    assert image[0x3A70A:0x3A70D] == bytes.fromhex('4531ff')
    assert image[0x3A74A:0x3A74D] == bytes.fromhex('4489fe')
print('ok')
