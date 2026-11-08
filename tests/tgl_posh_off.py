#!/usr/bin/env python3
"""TGL POSH-off: source contract and donor replay.

Without an argument, check that the installer honours -wex-tgl-posh, qualifies
both parameter copies and every lea before patching, and only flips 0x1C->0x0C.
With the exact AppleIntelTGLGraphics binary, verify both
g_cInitGfxHwCtxParamsForGraphics copies hold flags 0x1C, the GL params hold
0x0C, the three lea sites load one of the copies, and user space is told
POSH is off (_kIntelSharedExFtrPoshEnable = 0).
"""
from pathlib import Path
import re, struct, subprocess, sys

root = Path(__file__).resolve().parents[1]
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()
body = xe[xe.index('static void tglInstallPOSHOff('):]
body = body[:body.index('\n}\n')]
assert body.index('-wex-tgl-posh') < body.index('applyLookupPatch')
assert body.index('memcmp(reinterpret_cast<void *>(address+copies[i]),before') < body.index('applyLookupPatch')
assert body.index('lea[2]==0x35') < body.index('applyLookupPatch')
assert 'before[12] {0,0,0x1C,0,0,0,0,0,0,0,0,0}, after[12] {0,0,0x0C,0,0,0,0,0,0,0,0,0}' in body
assert 'tglInstallPOSHOff(patcher,address,size);' in xe[xe.index('bool XE::processTGLAccelerator('):][:8000]
copies = [int(x, 16) for x in re.search(r'copies\[2\] \{0x([0-9A-F]+),0x([0-9A-F]+)\}', body).groups()]
loads = [int(x, 16) for x in re.search(r'loads\[3\] \{0x([0-9A-F]+),0x([0-9A-F]+),0x([0-9A-F]+)\}', body).groups()]

if len(sys.argv) > 1:
    donor = Path(sys.argv[1]); image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    def addrs(name): return sorted(int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == name)
    assert addrs('__ZL32g_cInitGfxHwCtxParamsForGraphics') == sorted(copies)
    for a in copies: assert image[a:a + 12] == bytes([0, 0, 0x1C] + [0] * 9)
    gl = addrs('__ZL34g_cInitGfxHwCtxParamsForGraphicsGL')
    assert gl and all(image[a + 2] == 0x0C for a in gl)
    for site in loads:
        assert image[site:site + 3] == bytes.fromhex('488d35')
        target = site + 7 + struct.unpack_from('<i', image, site + 3)[0]
        assert target in copies, hex(target)
    # intelCreateSharedDeviceSettingsDictionary: OSNumber::withNumber(0,16) is the POSH value.
    assert image[0x17966:0x1796F] == bytes.fromhex('31db31ffbe10000000')
print('ok')
