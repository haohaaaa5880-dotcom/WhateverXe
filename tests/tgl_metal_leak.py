#!/usr/bin/env python3
"""Graphics Metal driver command-buffer leak (#47): source contract and replay.

Without an argument, check that the cs_validate_range route only rewrites the
dealloc jne (75 0D -> 90 90) inside a page whose 48 bytes at 0xD70 match
exactly, after a successful native validation, with Lilu's write method; that
it is a long route (Lilu's own user patcher chains on top with
routeMultipleLong and panics on short routes); and that the opt-out is honoured.
With the exact AppleIntelTGLGraphicsMTLDriver binary, verify the context is
unique, lies in -[MTLIGAccelCommandBuffer dealloc] at file offset 0x7ED70, and
that replaying the page patch only changes those two bytes.
"""
from pathlib import Path
import re, subprocess, sys

root = Path(__file__).resolve().parents[1]
host = (root / 'WhateverXe/kern_host.cpp').read_text()
m = re.search(r'metalLeakContext\[48\] \{([^}]*)\}', host)
context = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', m.group(1)))
assert len(context) == 48 and context[0x17:0x19] == b'\x75\x0d'
assert 'constexpr size_t metalLeakPageOffset=0xD70,metalLeakJne=0x17;' in host
wrap = host[host.index('boolean_t metalLeakValidateRange('):host.index('bool metalLeakSerialize(')]
assert wrap.index('FunctionCast(metalLeakValidateRange,metalLeakOriginal)') < wrap.index('if(!ok || !data)return ok;') < wrap.index('memcmp(page+metalLeakPageOffset,metalLeakContext')
assert wrap.index('memcmp(') < wrap.index('setKernelWriting(true') < wrap.index('jne[0]=0x90;jne[1]=0x90;') < wrap.index('setKernelWriting(false')
assert 'at+PAGE_SIZE<=size' in wrap
inst = host[host.index('void installMetalLeakFix('):]
inst = inst[:inst.index('\n}\n')]
assert inst.index('-wex-tgl-no-mtlleakfix') < inst.index('routeMultipleLong(KernelPatcher::KernelID')
assert '"_cs_validate_range"' in inst and 'routeMultiple(' not in inst.replace('routeMultipleLong(', '')
pk = host[host.index('void XeHost::processKernel('):]
assert pk.index('active=profile.displayAdmitted();') < pk.index('if(active)installMetalLeakFix(patcher);')
assert 'onProcLoad' not in (root / 'WhateverXe/kern_xe.cpp').read_text()

if len(sys.argv) > 1:
    drv = Path(sys.argv[1]); image = drv.read_bytes()
    assert image.count(context) == 1 and image.index(context) == 0x7ED70 and 0x7ED70 & 0xFFF == 0xD70
    nm = subprocess.run(['nm', str(drv)], capture_output=True, text=True, check=True).stdout
    sym = {l.split(' ', 2)[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split(' ', 2)) == 3 and l[0] != ' '}
    assert sym['-[MTLIGAccelCommandBuffer dealloc]'] == 0x7ECBA
    page = bytearray(image[0x7E000:0x7F000])
    for at in range(0, len(image) - 0xFFF, 0x1000):   # replay the wrapper over every file page
        p = image[at:at + 0x1000]
        if p[0xD70 + 0x17:0xD70 + 0x19] == b'\x75\x0d' and p[0xD70:0xDA0] == context: assert at == 0x7E000
    page[0xD87:0xD89] = b'\x90\x90'
    diff = [i for i in range(0x1000) if page[i] != image[0x7E000 + i]]
    assert diff == [0xD87, 0xD88]
print('ok')
