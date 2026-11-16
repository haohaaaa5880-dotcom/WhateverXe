#!/usr/bin/env python3
"""TGL context-owner map and RING_PSMI_CTL restore: source contract and donor replay.

Without an argument, check that the owner hook qualifies the exact donor entry
before routing, always calls the native initializer first, and that both
RING_PSMI_CTL registers are in the render-domain restore extras. With the
exact AppleIntelTGLGraphics binary, verify the initializer address/prologue,
that native start code applies the wait-for-event workaround to both
registers (|= 0x10801080), and that neither native restore list covers them.
"""
from pathlib import Path
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
header = (root / 'WhateverXe/kern_tgl_context_owners.hpp').read_text()
recovery = (root / 'WhateverXe/kern_tgl_recovery.hpp').read_text()
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()

entry = int(re.search(r'contextInitEntry=0x([0-9A-F]+)', header).group(1), 16)
prologue = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', re.search(r'contextInitPrologue\[17\]\s*\{([^}]*)\}', header).group(1)))
assert len(prologue) == 17

install = xe[xe.index('bool tglInstallContextOwners('):]
install = install[:install.index('\n}\n')]
assert install.index('solveSymbol') < install.index('memcmp') < install.index('RouteRequest owner(init,tglContextInit,tglContextInitOrg)')
wrapper = xe[xe.index('bool tglContextInit('):xe.index('bool tglContextOwnersSerialize(')]
assert wrapper.index('FunctionCast(tglContextInit,tglContextInitOrg)') < wrapper.index('TGLContextOwners::note')
assert 'return ok;' in wrapper
assert 'tglInstallContextOwners(patcher,index,address,size);' in xe[xe.index('bool XE::processTGLAccelerator('):][:6000]
# Both RING_PSMI_CTL copies are restored (masked) across a render-domain GDRST.
domain = xe[xe.index('constexpr TGLDomainRegister tglDomainRegisters[] {'):]
domain = domain[:domain.index('};')]
assert '{0x2050,true}' in domain and '{0x1A050,true}' in domain
restore = xe[xe.index('void tglRestoreDomainRegisters('):]
restore = restore[:restore.index('\n}\n')]
assert '|0xFFFF0000' in restore
assert 'if (domain) tglRestoreDomainRegisters(streamer);' in xe

if len(sys.argv) > 1:
    donor = Path(sys.argv[1])
    image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    symbols = {line.split()[-1]: int(line.split()[0], 16) for line in nm.splitlines() if len(line.split()) == 3}
    init = '__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh'
    assert symbols[init] == entry and image[entry:entry + 17] == prologue
    # setAsyncSliceCount: orl $0x10801080, 0x2050(%rax) / 0x1a050(%rax).
    assert image[0x258A5:0x258AF] == bytes.fromhex('81885020000080108010')
    assert image[0x258CD:0x258D7] == bytes.fromhex('818850a0010080108010')
    # Native restore arrays (0x24-byte resetRegisterInfo, register at +0). The
    # donor has two file-static rcsRegisters copies; check every one of them.
    def regs(name, limit=32):
        out = []
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == name:
                addr = int(parts[0], 16)
                for i in range(limit):
                    reg = struct.unpack_from('<I', image, addr + i * 0x24)[0]
                    if not reg: break
                    out.append(reg)
        return out
    rcs, ccs = regs('__ZL12rcsRegisters'), regs('__ZL12ccsRegisters')
    assert 0x229C in rcs and 0x1A29C in ccs, ([hex(r) for r in rcs], [hex(r) for r in ccs])
    assert 0x2050 not in rcs and 0x1A050 not in ccs and 0x1A050 not in rcs
print('tgl_context_owners: PASS' + (' (donor)' if len(sys.argv) > 1 else ''))
