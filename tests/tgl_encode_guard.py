#!/usr/bin/env python3
"""Tiger Lake hardware-encode guard: source contract and optional donor replay.

Without an argument, check that every guarded site is routed only after all
sites qualify, that the opt-in argument skips routing and advertisement
changes, and that decode keys are never removed. With the exact
AppleIntelTGLGraphics binary, also verify every symbol address, prologue,
native failure-store witness and the MediaKernel info-register copy.
"""
from pathlib import Path
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
header = (root / 'WhateverXe/kern_tgl_encode_guard.hpp').read_text()
xe = (root / 'WhateverXe/kern_xe.cpp').read_text()


def hexbytes(text):
    return bytes(int(b, 16) for b in re.findall(r'0x([0-9A-Fa-f]{2})', text))


sites = re.findall(r'\{"(__Z[^"]+)",\s*0x([0-9A-F]+),0x([0-9A-F]+),(\d+),\{([^}]*)\}\}', header)
assert [s[0].split('Context')[1][:8] for s in sites] == ['Main21pa', 'Main25pr', 'Media19p', 'Media20p'], sites
order = re.search(r'enum Path : uint32_t \{([^}]*)\}', header).group(1)
assert [x.strip() for x in order.split(',')] == ['EncodingCommon', 'MediaKernel', 'VDENC', 'AVCPAK', 'Count']
prologue = hexbytes(re.search(r'prologue\[13\] \{([^}]*)\}', header).group(1))
assert prologue == bytes.fromhex('554889e54157415641554154 53'.replace(' ', ''))
info_copy = hexbytes(re.search(r'mediaKernelInfoCopy\[3\] \{([^}]*)\}', header).group(1))
info_copy_at = int(re.search(r'mediaKernelInfoCopyAt=0x([0-9A-F]+)', header).group(1), 16)
assert 'constexpr uint32_t streamFailedOffset=0x3c;' in header
for _, _, _, length, data in sites:
    witness = hexbytes(data)
    assert len(witness) == int(length) and witness.endswith(b'\x3c\x00'), witness

# Integration contract.
install = xe[xe.index('bool tglInstallEncodeGuard('):xe.index('bool XE::processTGLAccelerator(')]
assert install.index('checkKernelArgument("-wex-tgl-hwenc")') < install.index('qualify(')
assert install.index('ready=ready && TGLEncodeGuard::qualify') < install.index('if(ready) {')
assert install.count('tglEncodeReject<') == 4 and 'routeMultiple' in install
for path in ['EncodingCommon', 'MediaKernel', 'VDENC', 'AVCPAK']:
    assert f'{{sites[{path}].symbol,tglEncodeReject<{path}>}}' in install, path
# Wrappers never call native encode code and fail through the native flag only.
wrapper = xe[xe.index('template<TGLEncodeGuard::Path P> void tglEncodeReject'):xe.index('// Returns true when encode')]
assert 'TGLEncodeGuard::reject(info);' in wrapper and 'FunctionCast' not in wrapper and 'org' not in wrapper
process = xe[xe.index('bool XE::processTGLAccelerator('):]
assert 'tglEncodeGuardReady=tglInstallEncodeGuard(patcher,index,address,size);' in process[:4000]
adv = xe[xe.index('that->setProperty("IOGVAHEVCDecode", "2");'):]
adv = adv[:adv.index('TGL blit scratch parameter mismatch')]
assert 'if (!checkKernelArgument("-wex-tgl-hwenc"))' in adv
assert 'removeProperty("IOGVAHEVCEncodeCapabilities")' in adv
assert 'setProperty("IOGVAEncoderRestricted", true)' in adv
for decode in ['IOGVACodec', 'IOGVAHEVCDecode', 'IOGVAHEVCDecodeCapabilities', 'IOGVAScaler', 'IOGVABGRAEnc', 'IOVARendererID']:
    assert f'removeProperty("{decode}")' not in xe, decode

# Pure reject semantics: only byte 0x3c changes.
info = bytearray(range(0x40))
info[0x3c] = 1
before = bytes(info)
info[0x3c] = 0
assert bytes(info) == before[:0x3c] + b'\x00' + before[0x3d:]

if len(sys.argv) > 1:
    donor = Path(sys.argv[1])
    image = donor.read_bytes()
    nm = subprocess.run(['nm', str(donor)], capture_output=True, text=True, check=True).stdout
    symbols = {line.split()[-1]: int(line.split()[0], 16) for line in nm.splitlines() if len(line.split()) == 3}
    # MH_KEXT_BUNDLE: __TEXT starts at vmaddr 0 and file offset 0.
    magic, = struct.unpack_from('<I', image)
    assert magic == 0xFEEDFACF
    for name, entry, store, length, data in sites:
        entry, store = int(entry, 16), int(store, 16)
        assert symbols[name] == entry, (name, hex(symbols.get(name, 0)))
        assert image[entry:entry + len(prologue)] == prologue, name
        assert image[store:store + int(length)] == hexbytes(data), name
        assert entry < store < entry + 0x1000, name
    assert image[info_copy_at:info_copy_at + 3] == info_copy
    # Decode processors are distinct and untouched by the guard.
    for decode in ['__ZN24IGAccelVideoContextMedia23process_token_AVCDecodeER24IOAccelCommandStreamInfo',
                   '__ZN24IGAccelVideoContextMedia25process_token_CODECDecodeER24IOAccelCommandStreamInfo']:
        assert decode in symbols and decode not in header, decode
print('tgl_encode_guard: PASS' + (' (donor)' if len(sys.argv) > 1 else ''))
