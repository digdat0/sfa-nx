#!/usr/bin/env python3
"""Merge the shader and pipeline caches recorded on consoles (or Ryujinx) into the seed that ships next to the NRO.

  py -3 switch/builder/seed-caches.py OUTDIR INPUT...

Each INPUT is either
  - a copy of a console's sdmc:/switch/SFA-NX/cache folder (deko/dksh_local.bin, pipeline_cache_deko.db), or
  - an earlier seed folder (initial_dksh_cache.bin, initial_pipeline_cache.db), so seeds can grow over time.

Writes OUTDIR/initial_dksh_cache.bin (the compiled shaders, switch/deko/dksh_file.h format),
OUTDIR/initial_pipeline_cache.db (Aurora's pipeline recipes) and OUTDIR/seed.json (what went in).
With OUTDIR=switch/seed, switch/CMakeLists.txt packs both files into the NRO's RomFS, where the renderer
reads them at start (switch/deko/shader_cache.cpp, extern/aurora/lib/gfx/pipeline_cache.cpp), so a fresh
install or a deleted cache starts with every known shader compiled instead of a black screen.

Shader records are checked as the console checks them (CRC, layout). Every shader file must carry the same
compiler id (uam, Tint post-pass, GX config version); a file from an older build is skipped with a warning,
and the id of the newest input wins. The files hold generated shader code and GX state recipes only: no game
data.
"""
import hashlib, json, sqlite3, struct, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # repository root (switch/builder/ -> ..)
HEADER = struct.Struct('<4sIQ')
RECORD = struct.Struct('<BII')
KIND_SHADER, KIND_MODULE, KIND_NAMED, KIND_STAGE = 1, 2, 3, 4
STAGES = (0, 4)  # ShaderStage: vertex, fragment
DKSH_NAMES = ('initial_dksh_cache.bin', 'deko/dksh_local.bin', 'dksh_local.bin')
DB_NAMES = ('initial_pipeline_cache.db', 'pipeline_cache_deko.db')


# How an input is named in seed.json: relative to the repository when inside it, otherwise only its last three
# path parts (e.g. cache1/deko/dksh_local.bin), so the committed seed.json never records a local folder.
def shown(path):
    path = Path(path).resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return '/'.join(path.parts[-3:])


# Reads one SWDK shader cache file: returns (compiler id, [(record key, kind, payload)], file info), or
# None if it is not a version-1 SWDK file. Records with a bad CRC or a cut-off end are dropped.
def read_dksh(path):
    data = path.read_bytes()
    if len(data) < HEADER.size:
        return None
    magic, version, compiler = HEADER.unpack_from(data, 0)
    if magic != b'SWDK' or version != 1:
        print(f'warning: {path}: not a version-1 SWDK file, skipped')
        return None
    records, bad, i = [], 0, HEADER.size
    while i + RECORD.size <= len(data):
        kind, size, crc = RECORD.unpack_from(data, i)
        if i + RECORD.size + size > len(data):
            break  # cut short: the file ends here
        payload = data[i + RECORD.size:i + RECORD.size + size]
        i += RECORD.size + size
        if zlib.crc32(payload, zlib.crc32(bytes([kind]))) != crc:
            bad += 1
            continue
        key = record_key(kind, payload)
        if key is not None:
            records.append((key, kind, payload))
    return compiler, records, {'path': shown(path), 'bytes': len(data), 'bad_crc': bad, 'torn_bytes': len(data) - i,
                               'mtime': path.stat().st_mtime}


def record_key(kind, p):
    """The record's identity, or None for one read_dksh_file would skip."""
    if kind == KIND_SHADER and len(p) >= 9 and p[0] in STAGES:
        return ('shader', p[0], p[1:9])
    if kind == KIND_MODULE and len(p) == 24:
        return ('module', p[0:8])
    if kind in (KIND_NAMED, KIND_STAGE) and len(p) >= 2 and p[0] in STAGES:
        n = p[1]
        if len(p) < 2 + n + 9:
            return None
        stride = 18 if kind == KIND_STAGE else 4
        if len(p) != 2 + n + 9 + stride * p[2 + n + 8]:
            return None
        return ('named', p[2:2 + n])
    return None


# The first of `names` that exists in `folder` (inputs may be a cache folder or an older seed folder).
def first_existing(folder, names):
    for n in names:
        if (folder / n).is_file():
            return folder / n
    return None


# Merges shader cache files into one: one record per shader (a compiled one beats a failed one), per
# module, and the last record per stage name. Returns the summary written to seed.json.
def pack_dksh(out, paths):
    files = [(p, r) for p in paths if (r := read_dksh(p)) is not None]
    if not files:
        return None
    # The newest input's compiler id wins (the build the seed is for).
    compiler = max(files, key=lambda f: f[1][2]['mtime'])[1][0]
    shaders, modules, named, meta = {}, {}, {}, []
    for path, (cid, records, info) in files:
        if cid != compiler:
            print(f'warning: {path}: compiler id {cid:016x}, not {compiler:016x}: from another build, skipped')
            continue
        meta.append(info)
        for key, kind, payload in records:
            if key[0] == 'shader':
                old = shaders.get(key)
                if old is None or (len(old[1]) == 9 and len(payload) > 9):  # a compiled one replaces a failed one
                    shaders[key] = (kind, payload)
            elif key[0] == 'module':
                modules.setdefault(key, (kind, payload))
            else:
                named.pop(key, None)  # the last record of each name wins, as on the console
                named[key] = (kind, payload)
    body = bytearray(HEADER.pack(b'SWDK', 1, compiler))
    for kind, payload in list(shaders.values()) + list(modules.values()) + list(named.values()):
        body += RECORD.pack(kind, len(payload), zlib.crc32(payload, zlib.crc32(bytes([kind])))) + payload
    out.write_bytes(body)
    compiled = sum(1 for _, p in shaders.values() if len(p) > 9)
    return {'compiler_id': f'{compiler:016x}', 'shaders': compiled, 'failed': len(shaders) - compiled,
            'modules': len(modules), 'binding_records': len(named), 'bytes': len(body),
            'sha256': hashlib.sha256(body).hexdigest(), 'inputs': meta}


# Merges Aurora pipeline databases (same schema only): for each pipeline the newest config version, and the
# earliest first-use frame, which is the order the game loads them at startup.
def pack_db(out, paths):
    if not paths:
        return None
    tmp = out.with_suffix('.tmp')
    tmp.unlink(missing_ok=True)
    dst = sqlite3.connect(tmp)
    schema, meta, rows = None, [], {}
    for p in paths:
        src = sqlite3.connect(f'file:{p.as_posix()}?mode=ro', uri=True)
        try:
            src.execute('PRAGMA quick_check').fetchone()
            s = src.execute('SELECT value FROM aurora_schema').fetchone()[0]
            if schema is None:
                schema = s
                for (sql,) in src.execute("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL"):
                    dst.execute(sql)
            elif s != schema:
                print(f'warning: {p}: schema {s}, not {schema}: skipped')
                continue
            n = 0
            for row in src.execute('SELECT type, hash, config_version, config_size, config, first_frame_used '
                                   'FROM pipeline_cache'):
                key = (row[0], row[1])
                old = rows.get(key)
                if old is None or row[2] > old[2] or (row[2] == old[2] and row[5] < old[5]):
                    rows[key] = row  # the newest config version; the earliest first use (load order)
                n += 1
            meta.append({'path': shown(p), 'recipes': n})
        except sqlite3.DatabaseError as e:
            print(f'warning: {p}: {e}: skipped')
        finally:
            src.close()
    if schema is None:
        dst.close()
        tmp.unlink(missing_ok=True)
        return None
    dst.execute('INSERT INTO aurora_schema VALUES (?)', (schema,))
    dst.executemany('INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?)', rows.values())
    dst.commit()
    dst.execute('VACUUM')
    dst.close()
    out.unlink(missing_ok=True)
    tmp.rename(out)
    return {'schema': schema, 'recipes': len(rows), 'bytes': out.stat().st_size, 'inputs': meta}


# Command line: OUTDIR INPUT... (see the top of the file).
def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    outdir = Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    folders = [Path(a) for a in sys.argv[2:]]
    for f in folders:
        if not f.is_dir():
            sys.exit(f'{f}: not a folder')
    dksh = [p for f in folders if (p := first_existing(f, DKSH_NAMES))]
    dbs = [p for f in folders if (p := first_existing(f, DB_NAMES))]
    # Read inputs before writing: OUTDIR may be one of them.
    staged = outdir / '.staging'
    staged.mkdir(exist_ok=True)
    summary = {'dksh': pack_dksh(staged / 'initial_dksh_cache.bin', dksh),
               'pipelines': pack_db(staged / 'initial_pipeline_cache.db', dbs)}
    for name in ('initial_dksh_cache.bin', 'initial_pipeline_cache.db'):
        if (staged / name).exists():
            (outdir / name).unlink(missing_ok=True)
            (staged / name).rename(outdir / name)
    staged.rmdir()
    (outdir / 'seed.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    d, p = summary['dksh'], summary['pipelines']
    print(f"shaders: {d['shaders'] if d else 0} compiled ({d['failed'] if d else 0} failed), "
          f"{d['modules'] if d else 0} modules; pipeline recipes: {p['recipes'] if p else 0}; written to {outdir}")


if __name__ == '__main__':
    main()
