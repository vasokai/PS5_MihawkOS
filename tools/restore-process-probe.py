#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Restore what an interrupted tools/run-process-probe.py run left deployed. No launch.

That runner restores the production title and runtime modules only while the console is
idle; a run whose title had to be killed leaves them replaced ("Console busy; signed production
backups retained"). Once the console is idle again, this puts back the signed production eboot and
the ntdll, wineserver, win32u and libvulkan modules from their backups, including each unsigned
`<name>.prx.elf` (firmware 12.00 and later maps that sibling, not the signed SELF), checks each
readback, and removes the probe's helper, programs, captures, broker results and child logs.

  tools/restore-process-probe.py --name RUN_NAME
"""
import argparse
import atexit
from ftplib import FTP
import importlib.util
import io
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT.parent / 'PS5_Vulkan/tools')]
import ps5_console
import console_guard
from console_log import redact
spec = importlib.util.spec_from_file_location('child_runner', ROOT / 'tools/run-child-probe.py')
child_runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(child_runner)
BASE = '/data/homebrew/PPSA99170/'
RUNTIME = '/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/'
PREFIX_DRIVE = '/data/ps5proton/prefixes/probe/drive_c/'
LOGS = '/data/ps5proton/logs/'


def verified(readback, signed, elf):
    if readback[:4] == b'\x7fELF':
        return child_runner.executable_sha(readback) == child_runner.executable_sha(elf)
    return readback == signed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True, help='the interrupted run; its deployment evidence is updated')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '-deployment.json')
    record = json.loads(evidence.read_text()) if evidence.exists() else {}
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise RuntimeError('Console busy; nothing restored')
    backup = Path(record.get('production_backup',ROOT / '.deps/wine-child-production'))
    production_image = (ROOT / 'dist/PPSA99170/eboot.bin').read_bytes()
    production_elf = (ROOT / 'build/ps5/eboot.elf').read_bytes()
    if production_image[:4] != bytes.fromhex('4f153d1d'):
        raise RuntimeError('Production title backup must be signed')
    console_guard.begin_write('probe', settings)   # end_write at exit, however main ends
    atexit.register(console_guard.end_write, settings)
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')

    def download(path, optional=False):
        data = io.BytesIO()
        try:
            ftp.retrbinary('RETR ' + path, data.write)
        except Exception as error:
            if not optional or not str(error).startswith('550'):
                raise
        return data.getvalue()

    def remove(path):
        if download(path, True):
            ftp.sendcmd('DELE ' + path)
        return not download(path, True)

    def names(directory):
        lines = []
        ftp.cwd(directory)
        ftp.retrlines('LIST', lines.append)
        return [line.split()[-1].rsplit('/', 1)[-1] for line in lines if line.split()]

    restoration = {}
    ftp.storbinary('STOR ' + BASE + 'eboot.bin', io.BytesIO(production_image))
    restoration['parent_restored'] = verified(download(BASE + 'eboot.bin'), production_image, production_elf)
    modules = {}
    for name in ('ntdll', 'wineserver', 'win32u', 'libvulkan'):
        original = (backup / (name + '.prx')).read_bytes()
        if original[:4] not in (bytes.fromhex('4f153d1d'), bytes.fromhex('5414f5ee')):
            raise RuntimeError('Production backup must be signed: ' + name)
        original_elf = (backup / (name + '.elf')).read_bytes()
        ftp.storbinary('STOR ' + RUNTIME + name + '.prx', io.BytesIO(original))
        ftp.storbinary('STOR ' + RUNTIME + name + '.prx.elf', io.BytesIO(original_elf))
        modules[name] = verified(download(RUNTIME + name + '.prx'), original, original_elf) and download(RUNTIME + name + '.prx.elf') == original_elf
    restoration['modules_restored'] = modules
    removed = {}
    # Production ships the helper loader and these probes (tools/build-production.sh): its copies go back.
    shipped = {BASE + 'wine-child.bin': backup / 'wine-child.bin' if (backup/'wine-child.bin').exists() else ROOT / 'dist/PPSA99170/wine-child.bin',
               PREFIX_DRIVE + 'process-probe.exe': ROOT / '.deps/staged/prefixes/probe/drive_c/process-probe.exe',
               PREFIX_DRIVE + 'graphics-render-probe.exe': ROOT / '.deps/staged/prefixes/probe/drive_c/graphics-render-probe.exe'}
    for path in (BASE + 'wine-child.bin', PREFIX_DRIVE + 'process-probe.exe', PREFIX_DRIVE + 'graphics-render-probe.exe',
                 LOGS + 'graphics-child-probe.ppm', BASE + 'gpu-broker-results.txt'):
        if path in shipped and shipped[path].is_file():
            data = shipped[path].read_bytes()
            ftp.storbinary('STOR ' + path, io.BytesIO(data))
            if path==BASE+'wine-child.bin':
                if data[:4]!=bytes.fromhex('4f153d1d'):raise RuntimeError('Helper restoration requires a signed SELF')
                removed['wine-child.bin']=verified(download(path),data,(backup/'wine-child.elf').read_bytes())
            else:removed[path.rsplit('/', 1)[-1]] = download(path) == data
        else:
            removed[path.rsplit('/', 1)[-1]] = remove(path)
    for name in names(BASE):
        if re.fullmatch(r'gpu-child-\d+-\d+\.ppm|wine-child-\d+\.log', name):
            removed[name] = remove(BASE + name)
    for name in names(LOGS):
        if re.fullmatch(r'graphics-broker-\d+\.ppm', name):
            removed[name] = remove(LOGS + name)
    prior_restored={}
    for path,item in record.get('prior_captures',{}).items():
        import hashlib
        data=Path(item['backup']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise RuntimeError('Prior capture backup differs')
        ftp.storbinary('STOR '+path,io.BytesIO(data));prior_restored[path]=download(path)==data
    restoration['prior_captures_restored']=prior_restored
    # Restore exact saved synthetic files; never replace a signed helper with
    # FTP's decoded ELF and never discard a payload image that predates the run.
    prior_files_restored={}
    for path,item in record.get('prior_files',{}).items():
        import hashlib
        data=Path(item['backup']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise RuntimeError('Prior file backup differs')
        ftp.storbinary('STOR '+path,io.BytesIO(data))
        prior_files_restored[path]=verified(download(path),data,Path(item['elf_backup']).read_bytes()) if 'elf_backup' in item else download(path)==data
    restoration['prior_files_restored']=prior_files_restored
    if record.get('payload_uploaded'):
        removed['payload-owner-ready']=remove(PREFIX_DRIVE+'payload-owner-ready')
        path='/data/ps5proton/run/proton-payload-child.elf'
        removed['payload_image']=prior_files_restored[path] if path in prior_files_restored else remove(path)
    ftp.quit()
    restoration['removed'] = removed
    restoration['production_restored'] = restoration['parent_restored'] and all(modules.values())
    restoration['probe_files_removed'] = all(removed.values()) and all(prior_restored.values()) and all(prior_files_restored.values())
    record['later_restoration'] = restoration
    if evidence.exists():
        evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(restoration, indent=2))
    return 0 if restoration['production_restored'] and restoration['probe_files_removed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaseException as error:
        if isinstance(error, SystemExit) and isinstance(error.code, int):
            raise
        print('Restoration failed: ' + redact(error), file=sys.stderr)
        sys.exit(1)
