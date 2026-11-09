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
assert body.index('target(queueLoad)==address+queueCopy') < body.index('applyLookupPatch')
assert body.index('target(initLoads[0])==address+initCopy') < body.index('applyLookupPatch')
# Only the command-queue copy is patched; the init/golden copy must keep POSH.
assert body.count('applyLookupPatch') == 1 and 'address+queueCopy);' in body
assert 'before[12] {0,0,0x1C,0,0,0,0,0,0,0,0,0}, after[12] {0,0,0x0C,0,0,0,0,0,0,0,0,0}' in body
assert 'tglInstallPOSHOff(patcher,address,size);' in xe[xe.index('bool XE::processTGLAccelerator('):][:8000]
m = re.search(r'queueCopy=0x([0-9A-F]+), initCopy=0x([0-9A-F]+), queueLoad=0x([0-9A-F]+), initLoads\[2\] \{0x([0-9A-F]+),0x([0-9A-F]+)\}', body)
queue, init, qload, il0, il1 = (int(x, 16) for x in m.groups())

# Reset-path repair inside the restoreFromSafeImage route: snapshot before the
# native copy, only on the beginReset path, and only with a qualified LRC layout.
cps = xe[xe.index('static volatile uint32_t *tglPOSHImage('):xe.index('bool tglCPSSerialize(')]
assert "img[0x1008/4]!=0x2244 || img[0x10D4/4]!=0x21B0" in cps and "c[0x6C]!=0" in cps
restore = cps[cps.index('bool tglCPSRestore(void *context) {'):]
assert restore.index('saved=img[0x10D8/4]') < restore.index('FunctionCast(tglCPSRestore,tglCPSRestoreOriginal)(context)') < restore.index('tglPOSHRepair(context,reset,saved)')
assert 'const bool reset=caller!=tglCPSRestoreCaller;' in restore
repair = cps[cps.index('static void tglPOSHRepair('):cps.index('bool tglPOSHRepairSerialize(')]
assert 'if(reset)control=(control&0xFFEFFFEFu)|0x00100000u;' in repair and 'if(!reset || !saved)return;' in repair
assert 'tglPOSHRepairEnabled=cpsOK && !checkKernelArgument("-wex-tgl-no-poshfix");' in xe

if len(sys.argv) > 1:
    donor = Path(sys.argv[1]); image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    def addrs(name): return sorted(int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == name)
    assert addrs('__ZL32g_cInitGfxHwCtxParamsForGraphics') == sorted([queue, init])
    for a in (queue, init): assert image[a:a + 12] == bytes([0, 0, 0x1C] + [0] * 9)
    gl = addrs('__ZL34g_cInitGfxHwCtxParamsForGraphicsGL')
    assert gl and all(image[a + 2] == 0x0C for a in gl)
    def lea(site):
        assert image[site:site + 3] == bytes.fromhex('488d35')
        return site + 7 + struct.unpack_from('<i', image, site + 3)[0]
    assert lea(qload) == queue and lea(il0) == init and lea(il1) == init
    # Every RIP-relative lea in __text that resolves to either copy: the queue
    # copy has exactly one user (the command-queue lea), the init copy two.
    def users(addr):
        hits = []
        for i in range(0x3000, 0xA0000):
            if image[i] in (0x48, 0x4C) and image[i + 1] == 0x8D and (image[i + 2] & 0xC7) == 0x05:
                if i + 7 + struct.unpack_from('<i', image, i + 3)[0] == addr: hits.append(i)
        return hits
    assert users(queue) == [qload], [hex(x) for x in users(queue)]
    assert users(init) == [il0, il1], [hex(x) for x in users(init)]
    # intelCreateSharedDeviceSettingsDictionary: OSNumber::withNumber(0,16) is the POSH value.
    assert image[0x17966:0x1796F] == bytes.fromhex('31db31ffbe10000000')
    # restoreFromSafeImage callers: initWithOptions (7C12C, then its own POSH
    # fixups at 7C15E/7C169/7C250) and beginReset (7C919) with no fixups.
    assert image[0x7C12C] == 0xE8 and image[0x7C919] == 0xE8
    for site in (0x7C12C, 0x7C919): assert site + 5 + struct.unpack_from('<i', image, site + 1)[0] == 0x7C392
    assert image[0x7C15E:0x7C163] == bytes.fromhex('41f6456e10')
    assert image[0x7C169:0x7C16E] == bytes.fromhex('b8efffefff')
    assert image[0x7C25C:0x7C263] == bytes.fromhex('418987d8100000')
print('ok')
