#!/usr/bin/env python3
"""Pack the shader seed into the downloadable shader database, sfa-shaderdb.bin, plus its manifest.

  py -3 switch/builder/make-shaderdb.py OUTDIR [--seed switch/seed] [--version N] [--notes TEXT]

Reads the seed that switch/builder/seed-caches.py writes (initial_dksh_cache.bin, initial_pipeline_cache.db,
seed.json) and writes
  OUTDIR/sfa-shaderdb.bin   the database players copy next to the NRO or get from the in-app updater
  OUTDIR/sfa-shaderdb.json  its manifest: version, format, compiler id, counts, size, sha256. The updater reads
                            this from the fixed "shaderdb" GitHub release to decide whether to offer the bin.

--version defaults to the existing OUTDIR/sfa-shaderdb.json's version when the shaders and recipes are
unchanged (content_sha256), else today's date as YYYYMMDD00, or one more than the previous version when
that is already today's or later, so every changed database is newer than the last.

File layout (little endian; read by switch/src/shaderdb_switch.cpp, which must agree):
  header, 64 bytes
    char magic[8]       "SFASHDB\\0"
    u32  format         1 (FORMAT below); a reader refuses a newer format
    u32  header_size    64 + 64 * section_count
    u32  version        database version (YYYYMMDDNN); the updater offers a larger one
    u32  section_count
    u64  compiler_id    the DKSH section's compiler id (dksh_file.h); 0 when there is no DKSH section
    u32  shaders        compiled shaders in the DKSH section
    u32  recipes        pipeline recipes in the PIPE section
    u32  pipeline_schema Aurora pipeline_cache schema of the PIPE section
    u32  header_crc     CRC-32 of the whole header and section table with this field zero
    u8   reserved[16]
  section table, 64 bytes per section
    char tag[4]         "DKSH" compiled shaders (an SWDK file), "PIPE" Aurora pipeline recipes (an sqlite
                        database), "INFO" seed.json (what went in, as text)
    u32  codec          0 stored, 1 zlib
    u64  offset         from the start of the file
    u64  packed_size
    u64  size           after unpacking
    u8   sha256[32]     of the unpacked bytes

The DKSH section only helps a build with the same compiler id; any build can use the PIPE recipes (they
are GX state, compiled on the console when no DKSH matches). Only generated shader code and GX state
recipes are inside: no game data.
"""
import argparse
import datetime
import hashlib
import json
import sqlite3
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FORMAT = 1
MAGIC = b'SFASHDB\0'
HEADER = struct.Struct('<8sIIIIQIIII16s')
SECTION = struct.Struct('<4sIQQQ32s')
CODEC_STORED, CODEC_ZLIB = 0, 1
assert HEADER.size == 64 and SECTION.size == 64


# The version number (see --version above): the previous one when the content is unchanged, else today's
# YYYYMMDD00 or one past the previous, whichever is larger.
def next_version(manifest, content):
    today = int(datetime.date.today().strftime('%Y%m%d')) * 100
    try:
        prev = json.loads(manifest.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        prev = {}
    if prev.get('content_sha256') == content and prev.get('version'):
        return prev['version']
    return max(today, prev.get('version', 0) + 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('outdir', type=Path)
    ap.add_argument('--seed', type=Path, default=ROOT / 'switch' / 'seed')
    ap.add_argument('--version', type=int, help='database version (default: date based, see above)')
    ap.add_argument('--notes', default='', help='one line for the manifest (what changed)')
    args = ap.parse_args()

    dksh = (args.seed / 'initial_dksh_cache.bin').read_bytes()
    pipe = (args.seed / 'initial_pipeline_cache.db').read_bytes()
    info_path = args.seed / 'seed.json'
    info = info_path.read_bytes() if info_path.exists() else b'{}'
    magic, swdk_version, compiler_id = struct.unpack_from('<4sIQ', dksh, 0)
    if magic != b'SWDK' or swdk_version != 1:
        sys.exit(f'{args.seed}/initial_dksh_cache.bin is not a version-1 SWDK file')
    seed = json.loads(info.decode('utf-8'))
    shaders = int(seed.get('dksh', {}).get('shaders', 0))
    # Count and check the recipes straight from the database rather than trusting seed.json.
    db = sqlite3.connect(f'file:{args.seed / "initial_pipeline_cache.db"}?mode=ro', uri=True)
    recipes = db.execute('SELECT COUNT(*) FROM pipeline_cache').fetchone()[0]
    schema = int(db.execute('SELECT value FROM aurora_schema').fetchone()[0])
    db.close()

    args.outdir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.outdir / 'sfa-shaderdb.json'
    sections = [(b'DKSH', dksh), (b'PIPE', pipe), (b'INFO', info)]
    # Content identity for the version rule: the shaders and recipes (not seed.json, whose input list
    # changes with every merge).
    content = hashlib.sha256(hashlib.sha256(dksh).digest() + hashlib.sha256(pipe).digest()).hexdigest()
    version = args.version or next_version(manifest_path, content)
    table_size = SECTION.size * len(sections)
    header_size = HEADER.size + table_size
    packed = [zlib.compress(data, 9) for _, data in sections]
    entries, offset = [], header_size
    for (tag, data), blob in zip(sections, packed):
        entries.append(SECTION.pack(tag, CODEC_ZLIB, offset, len(blob), len(data), hashlib.sha256(data).digest()))
        offset += len(blob)

    def header(crc):
        return HEADER.pack(MAGIC, FORMAT, header_size, version, len(sections), compiler_id, shaders, recipes, schema,
                           crc, b'\0' * 16)

    table = b''.join(entries)
    crc = zlib.crc32(header(0) + table)
    out = header(crc) + table + b''.join(packed)
    bin_path = args.outdir / 'sfa-shaderdb.bin'
    bin_path.write_bytes(out)

    manifest = {
        'name': 'sfa-shaderdb.bin',
        'version': version,
        'format': FORMAT,
        'compiler_id': f'{compiler_id:016x}',
        'shaders': shaders,
        'recipes': recipes,
        'pipeline_schema': schema,
        'size': len(out),
        'sha256': hashlib.sha256(out).hexdigest(),
        'content_sha256': content,
        'notes': args.notes,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'{bin_path}: version {version}, {shaders} shaders (compiler {compiler_id:016x}), {recipes} recipes, '
          f'{len(out) / 1048576:.2f} MiB')


if __name__ == '__main__':
    main()
