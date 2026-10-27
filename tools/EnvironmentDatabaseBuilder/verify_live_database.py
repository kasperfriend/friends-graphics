"""Read-only comparison against the actual client's loaded AreaTable/Map tables.

No injection, remote execution or writes. The supported table lookup globals
come from the fingerprinted executable's Area/Map lookup instructions.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path
from build_database import parse_dbc
from inspect_client import KNOWN_SHA256

# The following table-global addresses were read and compared only for these
# Ascension builds. A known 3.3.5 location-provider fingerprint is not enough
# to assume its live DBC globals or table contents match this layout.
TABLE_LAYOUT_SHA256 = {
    'e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6',
    'f4b9f6fce448194638c5b1c751483090a48c597c6272237f31b3d151b50d3114',
}


class ReadOnlyClient:
    def __init__(self, pid, executable):
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
        # VM_READ | QUERY_INFORMATION, no write/create-thread rights.
        self.handle = self.kernel.OpenProcess(0x410, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            path = ctypes.create_unicode_buffer(32768)
            size = ctypes.c_uint32(len(path))
            if not self.kernel.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(size)):
                raise ctypes.WinError(ctypes.get_last_error())
            if Path(path.value).resolve() != executable.resolve():
                raise ValueError('PID does not belong to the selected executable')
            psapi = ctypes.WinDLL('psapi', use_last_error=True)
            psapi.EnumProcessModulesEx.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
            modules = (ctypes.c_void_p * 512)()
            needed = ctypes.c_uint32()
            if not psapi.EnumProcessModulesEx(self.handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
                raise ctypes.WinError(ctypes.get_last_error())
            self.base = modules[0]
            if self.read(self.base, 2) != b'MZ':
                raise ValueError('Invalid main module base')
        except Exception:
            self.close()
            raise

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        if not 0 < address < 0x100000000 or not 0 < size <= 16 * 1024 * 1024:
            raise ValueError('Invalid memory range')
        buffer = ctypes.create_string_buffer(size)
        copied = ctypes.c_size_t()
        if not self.kernel.ReadProcessMemory(self.handle, ctypes.c_void_p(address), buffer, size, ctypes.byref(copied)) or copied.value != size:
            raise ctypes.WinError(ctypes.get_last_error())
        return buffer.raw

    def string(self, address):
        if not address:
            return ''
        result = bytearray()
        while len(result) < 1024:
            try:
                block = self.read(address + len(result), 16)
            except OSError:
                block = self.read(address + len(result), 1)
            stop = block.find(b'\0')
            if stop >= 0:
                return (result + block[:stop]).decode('utf-8')
            result.extend(block)
        raise ValueError('Unterminated native DBC string')

    def table(self, kind):
        # Area getter 0x519CA0 / flag getter 0x51A1D0, Map getter 0x519640.
        maximum_rva, minimum_rva, index_rva, fields, string_start = (
            (0x6D3140, 0x6D3144, 0x6D3154, 36, 11) if kind == 'AreaTable'
            else (0x6D416C, 0x6D4170, 0x6D4180, 66, 5))
        number = lambda rva: struct.unpack('<I', self.read(self.base + rva, 4))[0]
        maximum, minimum, index = number(maximum_rva), number(minimum_rva), number(index_rva)
        if maximum < minimum or maximum - minimum > 200000 or not index:
            raise ValueError('Native table lookup bounds unavailable')
        pointers = struct.unpack(f'<{maximum-minimum+1}I', self.read(index, (maximum-minimum+1)*4))
        result = []
        for relative_id, pointer in enumerate(pointers):
            if not pointer:
                continue
            row = struct.unpack(f'<{fields}I', self.read(pointer, fields*4))
            if row[0] != minimum + relative_id:
                raise ValueError('Native table record ID does not match its index')
            title = ''
            for value in row[string_start:string_start+16]:
                title = self.string(value)
                if title:
                    break
            if kind == 'AreaTable':
                result.append(dict(areaId=row[0], mapId=row[1], parentAreaId=row[2], flags=row[4], name=title))
            else:
                result.append(dict(mapId=row[0], name=title or self.string(row[1])))
        # Protect against table reload during the read; immutable DBC pointers
        # and bounds must remain identical for the entire snapshot.
        if (number(maximum_rva), number(minimum_rva), number(index_rva)) != (maximum, minimum, index):
            raise ValueError('Native table changed during snapshot')
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('client', type=Path)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path,
                        help='client executable to fingerprint (defaults to client/Ascension.exe)')
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--export-live', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/client-research/live-database-verification.json'))
    args = parser.parse_args()
    if args.executable:
        executable = args.executable
        if not executable.is_absolute() and not executable.exists():
            executable = args.client / executable
    else:
        executable = args.client / 'Ascension.exe'
    sha = hashlib.sha256(executable.read_bytes()).hexdigest()
    if sha not in KNOWN_SHA256:
        raise ValueError('Unsupported executable fingerprint; location getter signatures must be researched first')
    if sha not in TABLE_LAYOUT_SHA256:
        raise ValueError('Location fingerprint is recognized, but live table globals have not been verified for this executable')
    import pefile
    pe = pefile.PE(str(executable))
    checks = {0x119CA0: bytes.fromhex('a1 10 08 bd 00 8b 0d'), 0x119640: bytes.fromhex('a1 8c 08 bd 00 8b 0d')}
    if not all(pe.get_data(rva, len(value)) == value for rva, value in checks.items()):
        raise ValueError('Native lookup instruction evidence missing')
    import mpyq
    archive_path = args.archive or args.client/'Data/patch-M.MPQ'
    archive = mpyq.MPQArchive(str(archive_path), listfile=False)
    client = ReadOnlyClient(args.pid, executable)
    report = dict(executableSHA256=sha, pid=args.pid, archive=str(archive_path), verified=False, tables={})
    snapshots = {}
    try:
        for kind in ('AreaTable', 'Map'):
            payload = archive.read_file(f'DBFilesClient\\{kind}.dbc')
            expected = parse_dbc(payload, kind)
            actual = client.table(kind)
            key = 'areaId' if kind == 'AreaTable' else 'mapId'
            expected.sort(key=lambda row: row[key])
            actual.sort(key=lambda row: row[key])
            snapshots[kind] = actual
            matches = expected == actual
            structural_matches = [{k:v for k,v in r.items() if k != 'name'} for r in expected] == [{k:v for k,v in r.items() if k != 'name'} for r in actual]
            report['tables'][kind] = dict(archiveSHA256=hashlib.sha256(payload).hexdigest(), archiveRecords=len(expected), liveRecords=len(actual), matches=matches,
                structuralMatches=structural_matches,
                projectedDataSHA256=hashlib.sha256(json.dumps(actual, ensure_ascii=False, sort_keys=True).encode()).hexdigest())
            if not matches:
                expected_by_id = {r[key]: r for r in expected}
                report['tables'][kind]['differenceExamples'] = [r for r in actual if expected_by_id.get(r[key]) != r][:5]
            print(f'{kind}: archive={len(expected)}, live={len(actual)}, all runtime fields match={matches}')
        report['verified'] = all(t['structuralMatches'] for t in report['tables'].values())
    finally:
        client.close()
        archive.file.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    if not report['verified']:
        raise ValueError('Loaded tables differ from archive; provenance NOT confirmed')
    if args.export_live:
        quote = lambda value: '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
        lines = ['EnvironmentDatabase 1', '# provenance=verified-live-client-tables']
        lines += [f'M {r["mapId"]} {quote(r["name"])}' for r in snapshots['Map']]
        lines += [f'A {r["areaId"]} {r["mapId"]} {r["parentAreaId"]} {r["flags"]} {quote(r["name"])}' for r in snapshots['AreaTable']]
        args.export_live.parent.mkdir(parents=True, exist_ok=True)
        args.export_live.write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print('Actual loaded client table contents confirmed. No guessed MPQ priority required.')


if __name__ == '__main__':
    main()
