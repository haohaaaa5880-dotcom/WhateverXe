#!/usr/bin/env python3
"""TGL orphaned-stamp retirement: source contract and donor/family replay.

Without an argument, check that the opt-out is honoured, that both donor
entries are qualified before routing, that the reason capture is installed
before the eventTimeout route and only records on the capturing thread, that
the wrapper leaves calls outside restart_channel untouched, and that waiters
are woken through the machine's own signalStamp only after a retirement.

With the exact AppleIntelTGLGraphics binary, verify both prologues, that
eventTimeout reports reason 4 through encodeDebugInfo and returns no channel
on that path, and that the donor vtable leaves slots 0x210/0x228/0x250 to the
family. If the running system's kernel collection holds IOAcceleratorFamily2,
also verify the three family code signatures and restart_channel's use of
0x90/0x94 and of vtable slot 0x220.
"""
from pathlib import Path
import re, struct, subprocess, sys

root = Path(__file__).resolve().parents[1]
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()
hdr = (root / 'WhateverXe/kern_tgl_orphan_stamp.hpp').read_text()

wrap = xe[xe.index('void *tglOrphanEventTimeout('):xe.index('bool tglOrphanSerialize(')]
assert wrap.index('if (!TGLOrphanStamp::restartingIndex(machine,index))') < wrap.index('tglOrphanCaptureThread,self')
assert wrap.index('tglOrphanCaptureThread,self') < wrap.index('void *channel=FunctionCast')
assert wrap.index('void *channel=FunctionCast') < wrap.index('tglOrphanCaptureThread,0')
assert wrap.index('TGLOrphanStamp::takeSnapshot(') < wrap.index('TGLOrphanStamp::retire(') < wrap.index('slotSignalStamp]')
assert 'if (channel && TGLOrphanStamp::takeSnapshot(' in wrap and '&tglOrphanSnapshot);' in wrap
assert wrap.index('if (r.outcome==TGLOrphanStamp::Outcome::Retired)') < wrap.index('slotSignalStamp]')
assert wrap.rstrip().endswith('return channel;\n}') or 'return channel;' in wrap[-40:]
cap = xe[xe.index('void tglOrphanReasonCapture('):xe.index('bool tglOrphanLayoutKnown(')]
assert '==reinterpret_cast<uintptr_t>(current_thread())' in cap and cap.index('tglOrphanReason=reason') < cap.index('FunctionCast(')
install = xe[xe.index('static void tglInstallOrphanStamp('):]
install = install[:install.index('\n}\n')]
assert install.index('-wex-tgl-no-orphanstamp') < install.index('routeMultiple')
assert install.index('eventTimeoutPrologue') < install.index('routeMultiple')
assert install.index('encodeDebugInfoPrologue') < install.index('routeMultiple')
assert install.index('RouteRequest capture(reasonSymbol') < install.index('RouteRequest route(timeoutSymbol')
assert 'tglInstallOrphanStamp(patcher,index,address,size);' in xe[xe.index('bool XE::processTGLAccelerator('):][:8000]
retire = hdr[hdr.index('inline Result retire('):]
order = ['restartingIndex(machine,index)', 'if (channel)', 'reason!=reasonNoContext && !alive', 'if (!layout)', 'index>=count', 'static_cast<int32_t>(submitted-returned)<=0', 'returned!=snap->returned[i]', '*stamps[index]=submitted']
assert [retire.index(x) for x in order] == sorted(retire.index(x) for x in order)

def arr(name):
    body = re.search(name + r'\[[^\]]*\] \{([^}]*)\}', hdr).group(1)
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', body))

if len(sys.argv) > 1:
    donor = Path(sys.argv[1]); image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    t = sym['__ZN19IGAccelEventMachine12eventTimeoutEi']; e = sym['__ZN16IntelAccelerator15encodeDebugInfoE15IGTimeoutReason']
    assert t == 0x15F22 and image[t:t + 23] == arr('eventTimeoutPrologue')
    assert e == 0x2E460 and image[e:e + 26] == arr('encodeDebugInfoPrologue')
    # reason-4 path: movl $4,%esi ; jmp to encodeDebugInfo call ; then xorl r15d,r15d ; movq r15,rax
    assert image[0x1609C:0x160A1] == bytes.fromhex('be04000000')
    # reason 3: scheduler vtable 0x150 (checkForProgress) true; reason 5: other context under 5 s
    assert image[0x15F9E:0x15FA3] == bytes.fromhex('be03000000') and image[0x15F8C:0x15F92] == bytes.fromhex('ff9050010000')
    assert image[0x160FD:0x16106] == bytes.fromhex('498b7e10be05000000')
    vts = sym['__ZTV12IGScheduler5'] + 0x10
    assert struct.unpack('<Q', image[vts + 0x150:vts + 0x158])[0] == sym['__ZN12IGScheduler516checkForProgressE10IGHwCsType']
    assert image[0x16106] == 0xE8 and t + 0 < 0x16106 and (0x1610B + struct.unpack('<i', image[0x16107:0x1610B])[0]) == e
    assert image[0x1610B:0x16111] == bytes.fromhex('4531ff4c89f8')
    # donor vtable: slots 0x210/0x228/0x250 bound to the family, 0x220 donor eventTimeout
    rel = subprocess.run(['otool', '-rv', str(donor)], capture_output=True, text=True, check=True).stdout
    vt = sym['__ZTV19IGAccelEventMachine'] + 0x10
    binds = {}
    for l in rel.splitlines():
        m = re.match(r'\s*([0-9a-f]+)\s+.*\s(\S+)$', l)
        if m: binds[int(m.group(1), 16) - vt] = m.group(2)
    assert binds[0x210] == '__ZN24IOAccelEventMachineFast216checkGPUProgressEv'
    assert binds[0x228] == '__ZN20IOAccelEventMachine211signalStampEij'
    assert binds[0x250] == '__ZN24IOAccelEventMachineFast224deviceTerminatedUnlockedEv'
    kc = Path('/System/Library/KernelCollections/SystemKernelExtensions.kc')
    if kc.exists():
        data = kc.read_bytes()
        for name, code in [('checkGPUProgressCode', arr('checkGPUProgressCode')), ('signalStampCode', arr('signalStampCode')), ('deviceTerminatedCode', arr('deviceTerminatedCode'))]:
            assert data.count(code) == 1, name
        # restart_channel: cmpl $2 on 0x94, then index 0x90 to vtable slot 0x218, later slot 0x220
        rc = bytes.fromhex('8b879400000083f802751a8bb390000000488b034889dfff9018020000')
        i = data.find(rc); assert i > 0 and data.count(rc) == 1
        assert data.find(bytes.fromhex('ff9020020000'), i, i + 0x80) > 0
print('ok')
