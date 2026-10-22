"""Offline evidence for candidate RVAs. Does NOT verify live location semantics."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import pefile
import capstone

KNOWN_SHA256 = {
    'f4b9f6fce448194638c5b1c751483090a48c597c6272237f31b3d151b50d3114',
    'e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6',
    # User-supplied ArdenWoW 3.3.5a fingerprint; inspect_client verifies the
    # same 12340 location getter signatures before reporting candidate RVAs.
    'ed62bd406dbe92cabe4b38759c630df25a8d3b02a2646e1b77f62cb7be273c1d',
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('exe', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/client-research'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    data = args.exe.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    report = dict(executable=str(args.exe), sha256=sha, timestamp=pe.FILE_HEADER.TimeDateStamp,
                  imageBase=base, imageSize=pe.OPTIONAL_HEADER.SizeOfImage,
                  verification='offline-only; not in-world verified', luaFunctions={})
    for name in ('GetZoneText', 'GetSubZoneText', 'GetCurrentMapAreaID'):
        offset = data.find(name.encode() + b'\0')
        if offset < 0:
            continue
        address = base + pe.get_rva_from_offset(offset)
        needle = struct.pack('<I', address)
        pos = 0
        references = []
        while True:
            pos = data.find(needle, pos)
            if pos < 0:
                break
            function = struct.unpack_from('<I', data, pos + 4)[0]
            section = pe.get_section_by_rva(function - base)
            if section and section.Characteristics & 0x20000000:
                references.append(dict(tableRva=pe.get_rva_from_offset(pos), functionRva=function-base,
                                       instructions=[f'{i.address:#x} {i.mnemonic} {i.op_str}' for i in disassembler.disasm(pe.get_data(function-base,64),function)]))
            pos += 4
        report['luaFunctions'][name] = references
    if sha in KNOWN_SHA256:
        checks = {0x119640: bytes.fromhex('a1 8c 08 bd 00 8b 0d'),
                  0x119ca0: bytes.fromhex('a1 10 08 bd 00 8b 0d'),
                  0x1204e4: bytes.fromhex('89 35 0c 08 bd 00 a3 10 08 bd 00')}
        if not all(pe.get_data(rva, len(expected)) == expected for rva, expected in checks.items()):
            raise ValueError('Expected instruction evidence missing')
        report['candidateRvas'] = dict(map=0x7D088C, zone=0x7D080C, area=0x7D0810)
        report['candidateEvidence'] = {hex(rva): b.hex() for rva, b in checks.items()}
        (args.output / 'LocationProvider.diagnostic.ini').write_text(
            f'[LocationProvider]\nExeSHA256={sha}\nMapRva=0x7D088C\nZoneRva=0x7D080C\nAreaRva=0x7D0810\nVerifiedInWorld=0\n', encoding='utf-8')
    (args.output / 'client-inspection.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'SHA256={sha}. Wrote offline evidence to {args.output}. Live verification remains required.')


if __name__ == '__main__':
    main()
