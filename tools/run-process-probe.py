#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an isolated Wine child probe, restoring the production modules afterward.

Firmware 12.00 and later maps the unsigned sibling `<name>.prx.elf` (`pw_prx_elf_ps5_load_start`
appends `.elf` and never opens the signed SELF). A deploy replaces that file with the unsigned
module ELF and puts the production ELF back with the signed `.prx`.
"""
import argparse
from ftplib import FTP
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT.parent/'PS5_Vulkan/tools')]
import ps5_console
import console_guard
from console_log import redact
spec=importlib.util.spec_from_file_location('child_runner',ROOT/'tools/run-child-probe.py')
child_runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(child_runner)
BASE='/data/homebrew/PPSA99170/'
RUNTIME='/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/'
ACTIVE=None

def verified(readback, signed, elf):
    if readback[:4] == b'\x7fELF':
        return child_runner.executable_sha(readback) == child_runner.executable_sha(elf)
    return readback == signed

def payload_absence(settings,pids,probe):
    boundary='ProtonPayloadLifespan';body=b''
    for key,data in (('args',('payload-lifecycle '+' '.join(map(str,pids))).encode()),('pipe',b'1'),('elf',probe.read_bytes())):
        header=f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"'
        if key=='elf':header+='; filename="payload-lifecycle.elf"'
        body+=(header+'\r\n\r\n').encode()+data+b'\r\n'
    body+=f'--{boundary}--\r\n'.encode()
    request=urllib.request.Request('http://'+settings['host']+':8080/elfldr',data=body,headers={'Content-Type':'multipart/form-data; boundary='+boundary})
    with urllib.request.urlopen(request,timeout=20) as response:text=redact(response.read().decode(errors='replace'))
    states=[dict(zip(('pid','status','errno','absent'),map(int,m))) for m in re.findall(r'payload-lifecycle pid=(\d+) status=(-?\d+) errno=(\d+) absent=(\d+)',text)]
    return states

def main():
    global ACTIVE
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload',type=Path,help='receive the existing packet control endpoint in this payload ELF')
    parser.add_argument('--graphics-probe',type=Path,default=ROOT/'.deps/probes/graphics-render-probe.exe')
    parser.add_argument('--probe',type=Path,default=ROOT/'.deps/probes/process-probe.exe')
    parser.add_argument('--loader',required=True,type=Path)
    parser.add_argument('--name',required=True)
    parser.add_argument('--gpu-device-share',action='store_true',help='send the owner GPU device to the device-query helper; no rendering')
    parser.add_argument('--loading-screen',action='store_true',help='validate display handoff during the graphics child probe')
    parser.add_argument('--owner-build',type=Path,help='isolated wine-owner title build for the server-only bootstrap probe')
    parser.add_argument('--owner-graphics',action='store_true',help='reserve the owner for graphics even in a headless-only lifecycle case')
    parser.add_argument('--case',choices=('basic','suspended','terminate','repeat','parent-exit','disabled','descendants','descendants-repeat','descendants-parent-exit','java','graphics','graphics-broker','owner-failed','file-budget','program','payload-eight','payload-owner-close'),default='basic')
    parser.add_argument('--gpu-broker',action='store_true',help='owner GPU transport for every child and descendant')
    parser.add_argument('--gpu-client',type=Path,help='isolated broker Vulkan module build')
    parser.add_argument('--gpu-local',action='store_true',help='the broker offers device-local memory from its direct pool')
    parser.add_argument('--param-kernel',action='append',default=[],metavar='KEY=VALUE',
                        help='experiment: for this run only, the title\'s param.json gets "kernel": {KEY: VALUE} (e.g. flexibleMemorySize=1073741824); the original is put back and checked')
    parser.add_argument('--test-submit-failure',type=int,default=0,help='with --gpu-broker: the broker refuses the submission after this many, as a failed one; the run passes if the title survives it')
    parser.add_argument('--pad-script',type=Path,help='a pad script for the owner title (src/pw_pad_script.h); with --gpu-broker, every rendering child must see its held Y and left stick through XInput')
    parser.add_argument('--production-backup',type=Path,default=ROOT/'.deps/wine-child-production')
    parser.add_argument('--runtime-build',type=Path,default=ROOT/'.deps/wine-ps5')
    parser.add_argument('--profile',help='program: the library entry (/data/ps5proton/games/<id>) to run as the initial Windows program, with descendants')
    parser.add_argument('--program-seconds',type=int,default=180,help='program: seconds before the title is asked to stop the game')
    args=parser.parse_args()
    if args.case=='payload-owner-close' and (not args.payload or not args.owner_build or args.gpu_broker):parser.error('payload-owner-close requires a payload and a headless owner')
    if args.gpu_broker and (not args.owner_build or not args.gpu_client):parser.error('--gpu-broker requires --owner-build and --gpu-client')
    if args.gpu_local and not args.gpu_broker:parser.error('--gpu-local requires --gpu-broker')
    if args.case=='graphics-broker' and not args.gpu_broker:parser.error('--case graphics-broker requires --gpu-broker')
    if (args.case=='program')!=bool(args.profile):parser.error('--case program and --profile go together')
    if args.case=='program' and not args.owner_build:parser.error('--case program requires --owner-build')
    if args.case=='graphics' and not args.owner_build:parser.error('graphics requires the isolated owner title')
    if args.loading_screen and args.case!='graphics':parser.error('--loading-screen requires --case graphics')
    if args.owner_graphics and not args.owner_build:parser.error('--owner-graphics requires --owner-build')
    if args.case=='owner-failed' and not (args.owner_build and args.owner_graphics):parser.error('owner-failed requires --owner-build and --owner-graphics')
    if not re.fullmatch(r'[a-zA-Z0-9_-]+',args.name):parser.error('use a plain run name')
    evidence=ROOT/'evidence'/(args.name+'-deployment.json')
    if evidence.exists() or (ROOT/'evidence'/(args.name+'.json')).exists():raise RuntimeError('Use a new evidence name')
    ACTIVE=evidence
    loader=args.loader.resolve()
    manifest=json.loads((loader/'manifest.json').read_text())
    if args.gpu_device_share and not manifest.get('gpu_device_query'):parser.error('--gpu-device-share requires a device-query loader')
    owner_manifest=None
    if args.owner_build:
        owner_build=args.owner_build.resolve();owner_manifest=json.loads((owner_build/'manifest.json').read_text())
        if owner_manifest['kind']!='wine-owner':raise RuntimeError('Expected an isolated wine-owner title')
        owner_image=(owner_build/'source/dist/PPSA99170/eboot.bin').read_bytes()
        owner_elf=(owner_build/'build/eboot.elf').read_bytes()
        production_image=(ROOT/'dist/PPSA99170/eboot.bin').read_bytes()
        production_elf=(ROOT/'build/ps5/eboot.elf').read_bytes()
        if hashlib.sha256(owner_image).hexdigest()!=owner_manifest['eboot_sha256']:raise RuntimeError('Owner title differs from manifest')
        if production_image[:4]!=bytes.fromhex('4f153d1d'):raise RuntimeError('Production title backup must be signed')
    helper=(loader/'wine-child.bin').read_bytes()
    helper_elf=(loader/'wine-child.elf').read_bytes()
    if hashlib.sha256(helper).hexdigest()!=manifest['sha256'] or child_runner.helper_preload(helper_elf)!=int(manifest['preload_mask'],16):
        raise RuntimeError('Loader differs from its manifest')
    if args.gpu_broker and (not manifest.get('gpu_broker') or not owner_manifest.get('wine_gpu_broker')):raise RuntimeError('GPU broker requires matching owner and loader')
    prx=args.runtime_build.resolve()/'prx';backup=args.production_backup.resolve()
    report=json.loads((prx.parent/'report.json').read_text())
    uploads=[]
    for name in (('ntdll','wineserver','win32u') if args.gpu_broker else ('ntdll','wineserver')):
        entry=report['prx']['modules'][name]
        if not entry['built'] or any(entry.get(k) for k in ('unresolved','errors','title_unbound','raw_syscalls')):
            raise RuntimeError('Runtime import checks failed')
        image=(prx/'sce_module'/(name+'.prx')).read_bytes()
        if hashlib.sha256(image).hexdigest()!=entry['sha256']:raise RuntimeError('Runtime differs from its build report')
        original=(backup/(name+'.prx')).read_bytes()
        if original[:4] not in (bytes.fromhex('4f153d1d'),bytes.fromhex('5414f5ee')):raise RuntimeError('Production backup must be signed')
        uploads.append((name,image,(prx/(name+'.elf')).read_bytes(),original,(backup/(name+'.elf')).read_bytes()))
    if args.gpu_broker:
        client=args.gpu_client.resolve();client_inputs=json.loads((client/'manifest.json').read_text())
        image=(client/'gpu-client.prx').read_bytes();elf=(client/'client.elf').read_bytes()
        if not client_inputs.get('wsi') or hashlib.sha256(image).hexdigest()!=client_inputs['sha256'] or set(client_inputs['needed'])-{'libkernel.sprx','libSceLibcInternal.sprx'}:
            raise RuntimeError('Broker Vulkan module differs from manifest')
        uploads.append(('libvulkan',image,elf,(backup/'libvulkan.prx').read_bytes(),(backup/'libvulkan.elf').read_bytes()))
    payload_path='/data/ps5proton/run/proton-payload-child.elf'
    payload_image=args.payload.resolve().read_bytes() if args.payload else None
    if args.payload:
        payload_manifest=json.loads(args.payload.with_suffix('.json').read_text())
        if hashlib.sha256(payload_image).hexdigest()!=payload_manifest['sha256']:raise RuntimeError('Payload differs from manifest')
    settings=ps5_console.load_settings()
    def idle(): return ' count=0 ' in ps5_console.ps5vkctl_command(settings,'procs',timeout=15)+' '
    def connect():
        ftp=FTP()
        ftp.connect(settings['host'],settings['ftp_port'],timeout=30)
        ftp.login(settings['ftp_user'],settings['ftp_password'] or 'codex');return ftp
    def download(ftp,path,optional=False):
        data=io.BytesIO()
        try:ftp.retrbinary('RETR '+path,data.write)
        except Exception as e:
            if not optional or not str(e).startswith('550'):raise
        return data.getvalue()
    def names(ftp):
        lines=[]; ftp.retrlines('LIST',lines.append)
        return [line.split()[-1].rsplit('/',1)[-1] for line in lines if line.split()]
    def remove(ftp,path):
        result=ftp.sendcmd('DELE '+path)
        if not result.startswith(('226','250','200')):raise RuntimeError('Unexpected deletion result')
    if not idle():raise RuntimeError('Console busy; no deployment performed')
    record={'case':args.case,'loader':manifest,
            'runtime':{n:hashlib.sha256(img).hexdigest() for n,img,*_ in uploads},
            'runtime_elf':{n:hashlib.sha256(elf).hexdigest() for n,_,elf,*_ in uploads},
            'passed':False,'production_restored':False,'helper_removed':False,'status':'incomplete'}
    if args.payload:record['payload']=payload_manifest
    if args.gpu_device_share:record['gpu_device_share']=True
    if args.gpu_broker:record['gpu_broker']=True;record['gpu_client']=client_inputs
    if owner_manifest:record['owner_title']=owner_manifest;record['parent_restored']=False
    if args.loading_screen:record['loading_screen']=True
    evidence.write_text(json.dumps(record,indent=2)+'\n')
    deployed=[];helper_uploaded=False;probe_uploaded=False;java_uploaded=False;parent_deployed=False;graphics_uploaded=False;old_logs=set()
    param_deployed=False;original_param=b'';payload_uploaded=False
    graphics_path='/data/ps5proton/prefixes/probe/drive_c/graphics-render-probe.exe'
    capture_path='/data/ps5proton/logs/graphics-child-probe.ppm'
    broker_capture_names=[];broker_readback_names=[]
    production_files={};production_file_elfs={};prior_captures={}
    record['production_backup']=str(backup)
    try:
        # Synced and settled before run-console's launch (end_write when the block ends).
        with console_guard.writing('probe',settings),connect() as ftp:
            if owner_manifest and not verified(download(ftp,BASE+'eboot.bin'),production_image,production_elf):
                raise RuntimeError('Installed production title differs from signed backup')
            for name,_,_,original,original_elf in uploads:
                if not verified(download(ftp,RUNTIME+name+'.prx'), original, original_elf):
                    raise RuntimeError('Installed production module differs from signed local backup: '+name)
                # What a 12.00+ helper actually maps. A newer .prx beside an older .prx.elf does not load.
                if download(ftp,RUNTIME+name+'.prx.elf') != original_elf:
                    raise RuntimeError('Installed production module ELF differs from local backup: '+name+'.prx.elf')
            if args.case=='payload-owner-close' and download(ftp,'/data/ps5proton/prefixes/probe/drive_c/payload-owner-ready',True):raise RuntimeError('Owner-close readiness file already exists')
            # Production ships the helper loader and these probes (tools/build-production.sh): a run replaces them and
            # puts production's copies back afterwards; an older install had none, and the run removes its own.
            for path in (BASE+'wine-child.bin','/data/ps5proton/prefixes/probe/drive_c/process-probe.exe',graphics_path,*([payload_path] if args.payload else [])):
                existing=download(ftp,path,True)
                if existing:
                    if path==BASE+'wine-child.bin':
                        signed=(backup/'wine-child.bin').read_bytes();expected=(backup/'wine-child.elf').read_bytes()
                        if signed[:4]!=bytes.fromhex('4f153d1d') or not verified(existing,signed,expected):raise RuntimeError('Need the exact signed production helper backup')
                        production_files[path]=signed;production_file_elfs[path]=expected
                    else:production_files[path]=existing
            file_backup=ROOT/'build'/(args.name+'-prior-files');file_backup.mkdir(parents=True,exist_ok=True)
            for path,data in production_files.items():
                saved=file_backup/path.rsplit('/',1)[-1];saved.write_bytes(data)
                item={'backup':str(saved),'sha256':hashlib.sha256(data).hexdigest()}
                if path in production_file_elfs:
                    elf_saved=saved.with_suffix('.elf');elf_saved.write_bytes(production_file_elfs[path]);item['elf_backup']=str(elf_saved)
                record.setdefault('prior_files',{})[path]=item
            record['production_files']={path.rsplit('/',1)[-1]:hashlib.sha256(data).hexdigest() for path,data in production_files.items()}
            if args.case in ('graphics','graphics-broker'):
                if download(ftp,capture_path,True):raise RuntimeError('Synthetic graphics capture path already exists')
                graphics=args.graphics_probe.read_bytes()
                graphics_uploaded=True;record['graphics_uploaded']=True;record['graphics_sha256']=hashlib.sha256(graphics).hexdigest()
                evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+graphics_path,io.BytesIO(graphics))
                if download(ftp,graphics_path)!=graphics:raise RuntimeError('Graphics PE readback mismatch')
            if args.case=='java':
                work=ROOT/'.deps/java-process-validation'
                java_config=json.loads((work/'input.json').read_text())
                java_manifest=json.loads((work/'manifest.json').read_text())
                java_path=java_config['java_windows_path'].replace('\\','/').removeprefix('Z:')
                if hashlib.sha256(download(ftp,java_path)).hexdigest()!=java_manifest['sha256'][java_manifest['entrypoint']]:
                    raise RuntimeError('Installed JVM launcher differs from compilation manifest')
                console_java=download(ftp,java_path.rsplit('/bin/',1)[0]+'/bin/java.exe')
                if hashlib.sha256(console_java).hexdigest()!=java_manifest['sha256']['java.exe']:
                    raise RuntimeError('Installed descendant JVM launcher differs from compilation manifest')
                release=download(ftp,java_path.rsplit('/bin/',1)[0]+'/release')
                if hashlib.sha256(release).hexdigest()!=java_manifest['sha256']['release']:
                    raise RuntimeError('Console JVM release differs from compilation JDK')
                record['java_launchers_match_host']=True
                if download(ftp,BASE+'JavaProcessProbe.class',True):raise RuntimeError('Synthetic Java class already exists')
                java_class=(work/'classes/JavaProcessProbe.class').read_bytes()
                if hashlib.sha256(java_class).hexdigest()!=java_manifest['sha256']['JavaProcessProbe.class']:raise RuntimeError('Java class differs from manifest')
                java_uploaded=True;record['java_manifest']=java_manifest;record['java_uploaded']=True
                evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+BASE+'JavaProcessProbe.class',io.BytesIO(java_class))
                if download(ftp,BASE+'JavaProcessProbe.class')!=java_class:raise RuntimeError('Java class readback mismatch')
            ftp.cwd(BASE);old_logs=set(names(ftp))
            if args.gpu_broker:
                paths=[BASE+n for n in old_logs if re.fullmatch(r'gpu-child-\d+-\d+\.ppm',n)]
                if download(ftp,BASE+'gpu-broker-results.txt',True):paths.append(BASE+'gpu-broker-results.txt')
                ftp.cwd('/data/ps5proton/logs')
                paths+=['/data/ps5proton/logs/'+n for n in names(ftp) if re.fullmatch(r'graphics-broker-\d+\.ppm',n)]
                capture_backup=ROOT/'build'/(args.name+'-prior-captures');capture_backup.mkdir(parents=True,exist_ok=True)
                for path in paths:
                    data=download(ftp,path);saved=capture_backup/path.rsplit('/',1)[-1];saved.write_bytes(data)
                    prior_captures[path]=data
                    record.setdefault('prior_captures',{})[path]={'backup':str(saved),'sha256':hashlib.sha256(data).hexdigest()}
                    evidence.write_text(json.dumps(record,indent=2)+'\n')
                    remove(ftp,path)
                ftp.cwd(BASE)
            if args.param_kernel:
                # Only over the production param.json, byte for byte, so the original is known.
                original_param=download(ftp,BASE+'sce_sys/param.json')
                if original_param!=(ROOT/'dist/PPSA99170/sce_sys/param.json').read_bytes():
                    raise RuntimeError('Deployed param.json differs from the production build; not changed')
                param=json.loads(original_param)
                for item in args.param_kernel:
                    key,value=item.split('=',1);param.setdefault('kernel',{})[key]=int(value,0)
                changed=(json.dumps(param,indent=2)+'\n').encode()
                param_deployed=True;record['param_kernel']=param['kernel'];evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+BASE+'sce_sys/param.json',io.BytesIO(changed))
                if download(ftp,BASE+'sce_sys/param.json')!=changed:raise RuntimeError('Changed param.json readback differs')
            if owner_manifest:
                parent_deployed=True;record['parent_deployed']=True;evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+BASE+'eboot.bin',io.BytesIO(owner_image))
                if not verified(download(ftp,BASE+'eboot.bin'),owner_image,owner_elf):raise RuntimeError('Owner title readback mismatch')
            for name,image,expected,_,_ in uploads:
                deployed.append(name)
                record['deployed_modules']=list(deployed);evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+RUNTIME+name+'.prx',io.BytesIO(image))
                if not verified(download(ftp,RUNTIME+name+'.prx'), image, expected):
                    raise RuntimeError('Experimental module readback mismatch: '+name)
                ftp.storbinary('STOR '+RUNTIME+name+'.prx.elf',io.BytesIO(expected))
                if download(ftp,RUNTIME+name+'.prx.elf') != expected:
                    raise RuntimeError('Experimental module ELF readback mismatch: '+name+'.prx.elf')
            if args.payload:
                payload_uploaded=True;record['payload_uploaded']=True;evidence.write_text(json.dumps(record,indent=2)+'\n')
                ftp.storbinary('STOR '+payload_path,io.BytesIO(payload_image))
                if download(ftp,payload_path)!=payload_image:raise RuntimeError('Payload readback mismatch')
            helper_uploaded=True
            record['helper_uploaded']=True;evidence.write_text(json.dumps(record,indent=2)+'\n')
            ftp.storbinary('STOR '+BASE+'wine-child.bin',io.BytesIO(helper))
            if child_runner.executable_sha(download(ftp,BASE+'wine-child.bin'))!=child_runner.executable_sha(helper_elf):raise RuntimeError('Helper readback mismatch')
            probe=args.probe.read_bytes();probe_uploaded=True
            record['probe_uploaded']=True;evidence.write_text(json.dumps(record,indent=2)+'\n')
            ftp.storbinary('STOR /data/ps5proton/prefixes/probe/drive_c/process-probe.exe',io.BytesIO(probe))
            if download(ftp,'/data/ps5proton/prefixes/probe/drive_c/process-probe.exe')!=probe:raise RuntimeError('PE probe readback mismatch')
        record['deployment_verified']=True
        record['probe_sha256']=hashlib.sha256(probe).hexdigest()
        record['descendant_patch_sha256']=hashlib.sha256((ROOT/'wine/patches/0926-ntdll-ps5-broker-descendant-spawning.patch').read_bytes()).hexdigest()
        record['graphics_patch_sha256']=hashlib.sha256((ROOT/'wine/patches/0927-ntdll-ps5-owner-graphics-child.patch').read_bytes()).hexdigest()
        record['patch_sha256']=hashlib.sha256((ROOT/'wine/patches/0925-ntdll-ps5-service-managed-children.patch').read_bytes()).hexdigest()
        record['adapter_sha256']={name:hashlib.sha256((ROOT/'wine/ps5'/name).read_bytes()).hexdigest() for name in ('pw_wine_process.c','pw_wine_process.h','pw_wine_threads.c','pw_wine_threads.h','pw_wine_process_io.c','pw_wine_process_io.h')}
        evidence.write_text(json.dumps(record,indent=2)+'\n')
        command=[sys.executable,str(ROOT/'tools/run-console.py'),'--exe','process-probe.exe','--marker','PS5_PROTON_PROCESS_PROBE',
                 '--env','WINE_PS5_CHILDREN=1','--env','WINE_PS5_CHILD_METRICS=1','--env','WINE_PS5_NTSYNC=0','--env','WINE_PS5_SCHED=0',
                 '--env','PROTON_LOADING_SCREEN=0','--env','WINE_PS5_HID=0','--name',args.name,'--watchdog','100','--exit-stall','10']
        # Every helper is a payload process: the Sony-managed route was removed (--payload kept for old commands).
        if args.case in ('payload-eight','payload-owner-close'):command[command.index('--watchdog')+1]='300'
        if args.case=='disabled':
            opt=command.index('WINE_PS5_CHILDREN=1');del command[opt-1:opt+1]
        if args.gpu_device_share:command+=['--env','WINE_PS5_GPU_DEVICE_QUERY=1']
        if owner_manifest:
            command[command.index('--marker')+1]='PS5_PROTON_OWNER_BOOTSTRAP'
            command+=['--env','WINE_PS5_OWNER_BOOTSTRAP=1']
        if args.case=='payload-owner-close':
            command[command.index('--marker')+1]='PS5_PROTON_PAYLOAD_OWNER_CLOSE'
            command+=['--env','PROTON_PAYLOAD_TEST_OWNER_CLOSE=1']
        if args.case not in ('basic','program'):command+=['--args=--'+('graphics-console' if args.case=='graphics' else args.case)]
        if args.case=='file-budget':command[command.index('--watchdog')+1]='300'
        if args.case=='program':
            # The library entry's own executable, prefix and game.json, as the first Windows program; everything
            # it starts is a descendant through the title.
            at=command.index('--exe');command[at:at+2]=['--profile',args.profile,'--seconds',str(args.program_seconds)]
            command[command.index('--watchdog')+1]=str(args.program_seconds+150)
            command[command.index('--exit-stall')+1]='60'
            command+=['--env','WINE_PS5_DESCENDANTS=1']
        if args.case=='java':
            command += ['--env','PS5_PROCESS_JAVA='+java_config['java_windows_path'],
                        '--env','PS5_PROCESS_CLASSES=Z:'+BASE.rstrip('/').replace('/','\\')]
            command[command.index('--watchdog')+1]='240'
        if args.case=='graphics':
            command+=['--env','PS5_PROTON_PROBE_CAPTURE=Z:'+capture_path.replace('/','\\')]
            if args.loading_screen:command[command.index('PROTON_LOADING_SCREEN=0')]='PROTON_LOADING_SCREEN=1'
        if args.gpu_broker:command+=['--env','WINE_PS5_GPU_BROKER=1','--env','PROTON_GPU_CAPTURE=1']
        if args.gpu_local:command+=['--env','PROTON_GPU_LOCAL=1']
        if args.pad_script:command+=['--pad-script',str(args.pad_script.resolve())]
        if args.test_submit_failure:command+=['--env',f'PROTON_GPU_TEST_FAIL_SUBMIT={args.test_submit_failure}']
        if (args.case=='graphics' or args.owner_graphics) and not args.gpu_broker:command+=['--env','WINE_PS5_GRAPHICS_HOST=1']
        result=subprocess.run(command,cwd=ROOT)
        record['runner_status']=result.returncode
        record['passed']=result.returncode==0
        record['status']='complete'
        capture=(ROOT/'klog'/(args.name+'.log')).read_text(errors='replace')
        if args.case=='owner-failed':
            run=json.loads((ROOT/'evidence'/(args.name+'.json')).read_text())
            record['expected_initial_exit']=0xffffffff
            record['expected_failure_observed']=result.returncode==1 and run['result']=='finished' and run['probe_verdict']=='FAIL' and not run['exit_stall'] and \
                'PS5_PROTON_OWNER_BOOTSTRAP FAIL initial_status=1 graphics_claimed=0' in capture and 'completion=0xffffffff' in capture
            record['passed']=record['expected_failure_observed']
        record['post_launch_fault_records']=len(re.findall(r'A user thread receives a fatal signal|GPU_FAULT|gpu fault',capture,re.I))
        record['kernel_fmem']=sorted(set(re.findall(r'FMEM size: (0x[0-9a-f]+)',capture)))
        record['native_descriptor_samples']=[{'stage':stage,'count':int(count),'limit':int(limit)} for stage,count,limit in re.findall(r'child metrics stage=(\w+) native_fds=(\d+) limit=(\d+)',capture)]
        wine_text=(ROOT/'klog'/(args.name+'-wine.log')).read_text(errors='replace')
        record['windows_handle_samples']=[{'before':int(before),'after':int(after)} for before,after in re.findall(r'OutputDebugStringA "process-probe: parent handles before (\d+) after (\d+)',wine_text)]
        if args.case=='java':
            record['java_nodes']=[{'depth':int(d),'pid':int(p)} for d,p in re.findall(r'OutputDebugStringA "JAVA_NODE PASS depth=(\d+) pid=(\d+)',wine_text)]
            record['passed']=record['passed'] and len(record['java_nodes'])==3
        if args.case=='file-budget':
            # The Windows programs run in child processes: their output is in the child logs, fetched below.
            record['server_files']=[dict(zip(('live','parked','parks','unparks','limit_hits','failed'),map(int,m))) for m in
                re.findall(r'files live=(\d+) parked=(\d+) parks=(\d+) unparks=(\d+) limit=(\d+) failed=(\d+)',capture)]
            record['title_descriptors']=[dict(zip(('files','dirs'),map(int,m))) for m in re.findall(r'perf: descriptors open=\d+ of \d+ sockets=\d+ fifos=\d+ files=(\d+) dirs=(\d+)',capture)]
        record['native_children']=[{'pid':int(pid),'service_id':int(service,16)} for pid,service in
                                  re.findall(r'child ready native=(\d+) service=(0x[0-9a-f]+)',capture)]
        record['native_child_deletions']=[item['pid'] for item in record['native_children'] if
            'process delete event : pid='+hex(item['pid']) in capture]
        record['native_children_absent']=record['native_child_deletions']
        if args.payload:
            # Include a helper that died before READY: the rendezvous already
            # identified it and bootstrap failure must also prove absence.
            known={item['pid'] for item in record['native_children']}
            for pid in map(int,re.findall(r'payload rendezvous native=(\d+)',capture)):
                if pid not in known:record['native_children'].append({'pid':pid,'service_id':1});known.add(pid)
            states=payload_absence(settings,[item['pid'] for item in record['native_children']],args.payload.parent/'payload-lifecycle-probe.elf')
            record['payload_lifecycle']=states
            record['native_children_absent']=[item['pid'] for item in states if item['absent']]
        record['passed']=record['passed'] and not record['post_launch_fault_records'] and (bool(record['native_children']) if args.case!='disabled' else not record['native_children']) and \
                         len(record['native_children_absent'])==len(record['native_children'])
        with connect() as ftp:
            # Helpers log into this session's folder (src/pw_log_sessions.h), which the title made empty at its start.
            sessions='/data/ps5proton/logs/sessions/0/'
            ftp.cwd(sessions)
            logs=[name for name in names(ftp) if re.fullmatch(r'wine-child-\d+\.log',name)]
            record['child_logs']=logs
            for name in logs:
                text=redact(download(ftp,sessions+name).decode(errors='replace'))
                (ROOT/'klog'/(args.name+'-'+name)).write_text(text)
            if args.case=='payload-owner-close':
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['owner_close_with_eight_ready']='PS5_PROTON_PAYLOAD_OWNER_READY children=8' in child_text
                record['passed']=record['passed'] and record['owner_close_with_eight_ready'] and len(record['native_children_absent'])>=9
            if args.case=='payload-eight':
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['eight_simultaneous']= 'process-limit: payload simultaneous children (ready, alive) 8 8' in child_text
                record['passed']=record['passed'] and record['eight_simultaneous'] and 'PS5_PROTON_PROCESS_PROBE PASS' in child_text
            if args.case in ('file-budget','program'):
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['child_programs']=[{'log':n,'image':m[0] if m else None} for n in logs for m in
                    [re.findall(r'NtCreateUserProcess L"[^"]*" image L"([^"]*)"',(ROOT/'klog'/(args.name+'-'+n)).read_text())[:1] or
                     re.findall(r'wine-child: image (\S+)',(ROOT/'klog'/(args.name+'-'+n)).read_text())[:1]]]
            if args.case=='program':
                record['child_census']=[dict(zip(('seconds','files','dirs'),map(int,m))) for m in re.findall(r'wine-child: census seconds=(\d+) files=(\d+) dirs=(\d+)',child_text)]
                record['server_reopen_failures']=len(re.findall(r'fdv: cannot reopen',capture))
                record['server_protocol_errors']=len(re.findall(r'Protocol error: process',capture))
                record['too_many_open_files']=len(re.findall(r'Too many open files',capture+child_text))
                end=re.search(r'Wine: open files at the end: live=(\d+) parked=(\d+) parks=(\d+) unparks=(\d+) limit hits=(\d+) failed=(\d+)',capture)
                record['server_files_at_end']=dict(zip(('live','parked','parks','unparks','limit_hits','failed'),map(int,end.groups()))) if end else None
                # The program is stopped by the title at its budget; what counts is that nothing ran out of files.
                record['passed']=not record['post_launch_fault_records'] and bool(record['native_children']) and \
                    len(record['native_children_absent'])==len(record['native_children']) and \
                    not record['server_protocol_errors'] and not record['server_reopen_failures'] and not record['too_many_open_files']
            if args.case=='file-budget':
                record['file_budget']=re.findall(r'OutputDebugStringA "file-budget: ([^"]*?)\\n"',child_text)
                record['child_census']=[dict(zip(('seconds','files','dirs'),map(int,m))) for m in re.findall(r'wine-child: census seconds=(\d+) files=(\d+) dirs=(\d+)',child_text)]
                record['server_reopen_failures']=len(re.findall(r'fdv: cannot reopen',capture))
                record['server_protocol_errors']=len(re.findall(r'Protocol error: process',capture))
                end=re.search(r'Wine: open files at the end: live=(\d+) parked=(\d+) parks=(\d+) unparks=(\d+) limit hits=(\d+) failed=(\d+)',capture)
                record['server_files_at_end']=dict(zip(('live','parked','parks','unparks','limit_hits','failed'),map(int,end.groups()))) if end else None
                record['passed']=record['passed'] and 'summary (open handles, children passed) 450 3' in record['file_budget'] and \
                    'OutputDebugStringA "PS5_PROTON_PROCESS_PROBE PASS' in child_text and not record['server_protocol_errors']
            if args.gpu_device_share:
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                identity_pattern = r'gpu identity role=(\w+) pid=(\d+) abi=(0x[0-9a-f]+|0) app_status=(0x[0-9a-f]+|0) guard=(\d+) app=(\d+) header4=(0x[0-9a-f]+|0) header8=(0x[0-9a-f]+|0) header12=(0x[0-9a-f]+|0)'
                record['gpu_process_identity'] = [
                    dict(zip(('role','pid','abi','app_status','guard','app','header4','header8','header12'),
                             (role,int(pid),int(abi,16),int(status,16),int(guard),int(app),int(a,16),int(b,16),int(c,16))))
                    for role,pid,abi,status,guard,app,a,b,c in re.findall(identity_pattern,capture+'\n'+child_text)]
                record['gpu_child_queries']=[{'phase':p,'status':int(s),'errno':int(e),'value':int(v,16)}
                    for p,s,e,v in re.findall(r'gpu inherited (\w+) ioctl=(-?\d+) errno=(\d+) value=(0x[0-9a-f]+|0)',child_text)]
                record['gpu_owner_queries']=[{'status':int(s),'errno':int(e),'value':int(v,16)}
                    for s,e,v in re.findall(r'gpu donor ioctl=(-?\d+) errno=(\d+) value=(0x[0-9a-f]+|0)',capture)]
                record['gpu_query_passed']=bool(record['gpu_child_queries']) and bool(record['gpu_owner_queries']) and \
                    all(q['status']==0 for q in record['gpu_child_queries']+record['gpu_owner_queries'])
                record['passed']=record['passed'] and record['gpu_query_passed']
            if args.case.startswith('descendants'):
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['descendant_node_verdicts']=len(re.findall(r'OutputDebugStringA "PS5_PROTON_DESCENDANT_NODE PASS',child_text))
                record['broker_spawns']=[{'requester':int(a),'child':int(b),'status':int(c)} for a,b,c in re.findall(r'broker requester=(\d+) child=(\d+) status=(\d+)',capture)]
                expected=12 if args.case=='descendants-repeat' else 3
                record['passed']=record['passed'] and record['descendant_node_verdicts']==expected
                if args.case=='descendants-parent-exit':
                    record['descendant_survived_parent']='PS5_PROTON_DESCENDANT_SURVIVED_PARENT PASS' in child_text
                    record['passed']=record['passed'] and record['descendant_survived_parent']
            if args.case=='graphics':
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['windows_parent_passed']='PS5_PROTON_PROCESS_PROBE PASS' in child_text
                record['windows_handle_samples']=[{'before':int(a),'after':int(b)} for a,b in re.findall(r'OutputDebugStringA "process-probe: parent handles before (\d+) after (\d+)',child_text)]
                record['second_graphics_rejected']='PS5_PROTON_GRAPHICS_SECOND_REJECTED PASS' in child_text
                record['graphics_metadata_passed']='PS5_PROTON_GRAPHICS_CHILD PASS: Windows parent and inherited pipe' in wine_text
                record['graphics_teardown_passed']='PS5_PROTON_GRAPHICS_CHILD PASS: GPU initialized' in wine_text
                record['graphics_descendant_passed']='PS5_PROTON_GRAPHICS_CHILD PASS: headless descendant during GPU lifetime' in wine_text
                record['frame_count']=int(re.search(r'PS5_PROTON_D3D11_PROBE bench frames=(\d+)',wine_text).group(1)) if 'PS5_PROTON_D3D11_PROBE bench frames=' in wine_text else 0
                pixels=download(ftp,capture_path,True)
                if pixels:
                    (ROOT/'klog'/(args.name+'-readback.ppm')).write_bytes(pixels)
                header=b'P6\n1280 720\n255\n'
                record['readback_sha256']=hashlib.sha256(pixels).hexdigest() if pixels else None
                record['readback_bytes']=len(pixels)
                record['readback_format_passed']=pixels.startswith(header) and len(pixels)==len(header)+1280*720*3
                expected=[((10,10),(6,13,26)),((640,360),(127,64,64)),((640,100),(242,6,6)),((220,630),(8,19,228)),((1060,630),(8,228,19))]
                record['pixel_samples']=[]
                if record['readback_format_passed']:
                    for (x,y),color in expected:
                        offset=len(header)+(y*1280+x)*3;actual=list(pixels[offset:offset+3])
                        record['pixel_samples'].append({'x':x,'y':y,'rgb':actual,'expected':list(color),'passed':all(abs(a-b)<=8 for a,b in zip(actual,color))})
                record['readback_pixels_passed']=bool(record['pixel_samples']) and all(p['passed'] for p in record['pixel_samples'])
                if args.loading_screen:
                    record['display_handoff_passed']='loading screen: shown for' in capture and 'loading screen: closed after' in capture and "the game's presenter takes the display" in capture
                    record['passed']=record['passed'] and record['display_handoff_passed']
                record['passed']=record['passed'] and all(record[k] for k in ('windows_parent_passed','second_graphics_rejected','graphics_metadata_passed','graphics_teardown_passed','graphics_descendant_passed','readback_format_passed')) and record['frame_count']==180
                record['passed']=record['passed'] and record['readback_pixels_passed']
            if args.gpu_broker:
                child_text='\n'.join((ROOT/'klog'/(args.name+'-'+n)).read_text() for n in logs)
                record['windows_handle_samples']=[{'before':int(before),'after':int(after)}
                    for before,after in re.findall(r'OutputDebugStringA "process-probe: parent handles before (\d+) after (\d+)',child_text)]
                broker_text=redact(download(ftp,BASE+'gpu-broker-results.txt',True).decode(errors='replace'))
                (ROOT/'klog'/(args.name+'-gpu-broker.log')).write_text(broker_text)
                record['gpu_broker_results']=broker_text.splitlines()
                record['gpu_clients_closed']=[{'cookie':int(a),'pid':int(b),'frames':int(c),'submissions':int(d),'backing_released':int(e)}
                    for a,b,c,d,e in re.findall(r'gpu-wine cookie=(\d+) pid=(-?\d+) closed=1 absent=1 frames=(\d+) submissions=(\d+) backing_released=(\d+)',broker_text)]
                record['gpu_transport_errors']=len(re.findall(r'gpu-wine .*operation=\d+ status=(?!0(?: |$))',broker_text))
                record['gpu_allocation_failures']=[{'cookie':int(cookie),'pid':int(pid),'stage':stage,'bytes':int(size),'flexible_status':int(status,0),'flexible_available':int(available)}
                    for cookie,pid,stage,size,status,available in re.findall(r'allocation_failed=1 cookie=(\d+) pid=(\d+) stage=(\w+) bytes=(\d+) flexible_status=(\w+) flexible_available=(\d+)',broker_text)]
                live={};peak=0
                for line in broker_text.splitlines():
                    sample=re.search(r'gpu-wine cookie=(\d+).*live_bytes=(\d+)',line)
                    closed=re.search(r'gpu-wine cookie=(\d+).*closed=1',line)
                    if sample:live[int(sample[1])]=int(sample[2])
                    if closed:live.pop(int(closed[1]),None)
                    peak=max(peak,sum(live.values()))
                # Per-operation lines exist only when the broker traces; its stop line carries the peaks either way.
                peaks=re.search(r'gpu-wine stopped=1 .*host_peak=(\d+) local_peak=(\d+)',broker_text)
                record['gpu_shared_peak_bytes']=int(peaks[1]) if peaks else peak
                record['gpu_local']=args.gpu_local
                record['gpu_local_offers']=[int(v) for v in re.findall(r'gpu-wine cookie=\d+ pid=\d+ local_offer=(\d+)',broker_text)]
                record['gpu_local_peaks']=[{'cookie':int(a),'pid':int(b),'local_peak':int(c)} for a,b,c in
                    re.findall(r'gpu-wine cookie=(\d+) pid=(-?\d+) closed=1 .* local_peak=(\d+)',broker_text)]
                live_local={};local_peak=0
                for line in broker_text.splitlines():
                    sample=re.search(r'gpu-wine cookie=(\d+).*local_bytes=(\d+)',line)
                    closed=re.search(r'gpu-wine cookie=(\d+).*closed=1',line)
                    if sample:live_local[int(sample[1])]=int(sample[2])
                    if closed:live_local.pop(int(closed[1]),None)
                    local_peak=max(local_peak,sum(live_local.values()))
                record['gpu_local_total_peak_bytes']=int(peaks[2]) if peaks else local_peak
                minimum=re.search(r'gpu-wine stopped=1 .*flexible_min=(\d+) direct_min=(\d+)',broker_text)
                record['gpu_flexible_min']=int(minimum[1]) if minimum else None
                record['gpu_direct_min']=int(minimum[2]) if minimum else None
                if args.gpu_local and args.case not in ('file-budget','program'):  # not a fixed renderer
                    record['passed'] &= bool(record['gpu_local_offers']) and all(v>0 for v in record['gpu_local_offers']) and \
                        any(c['local_peak']>0 for c in record['gpu_local_peaks'])
                record['bootstrap_threads']=[{'log':n,'stack_bytes':int(size),'status':int(status)} for n in logs
                    for size,status in re.findall(r'bootstrap stack_bytes=(\d+) thread_status=(\d+)',(ROOT/'klog'/(args.name+'-'+n)).read_text())]
                record['passed'] &= 'gpu-wine stopped=1' in broker_text and not record['gpu_transport_errors']
                record['broker_wedged']=re.findall(r'gpu-broker wedged=1 reason=(\S+)',broker_text)
                ftp.cwd(BASE);broker_capture_names=[n for n in names(ftp) if n not in old_logs and re.fullmatch(r'gpu-child-\d+-\d+\.ppm',n)]
                record['gpu_display_captures']=[]
                expected=[((10,10),(6,13,26)),((640,360),(127,64,64)),((640,100),(242,6,6)),((220,630),(8,19,228)),((1060,630),(8,228,19))]
                def readback_pixels(data,width,height):
                    header=f'P6\n{width} {height}\n255\n'.encode()
                    okay=data.startswith(header) and len(data)==len(header)+width*height*3;samples=[]
                    if okay:
                        for (x,y),rgb in expected:
                            offset=len(header)+((y*height//720)*width+x*width//1280)*3;actual=list(data[offset:offset+3])
                            samples.append({'rgb':actual,'expected':list(rgb),'passed':all(abs(a-b)<=8 for a,b in zip(actual,rgb))})
                    return okay,samples,bool(samples) and all(q['passed'] for q in samples)
                for n in broker_capture_names:
                    data=download(ftp,BASE+n);okay,samples,correct=readback_pixels(data,1920,1080)
                    (ROOT/'klog'/(args.name+'-'+n)).write_bytes(data)
                    record['gpu_display_captures'].append({'name':n,'sha256':hashlib.sha256(data).hexdigest(),'format_passed':okay,'samples':samples,'pixels_passed':correct})
                ftp.cwd('/data/ps5proton/logs');broker_readback_names=[n for n in names(ftp) if re.fullmatch(r'graphics-broker-\d+\.ppm',n)]
                record['gpu_readbacks']=[]
                for n in broker_readback_names:
                    data=download(ftp,'/data/ps5proton/logs/'+n);okay,samples,correct=readback_pixels(data,640,360)
                    (ROOT/'klog'/(args.name+'-'+n)).write_bytes(data)
                    record['gpu_readbacks'].append({'name':n,'sha256':hashlib.sha256(data).hexdigest(),'format_passed':okay,'samples':samples,'pixels_passed':correct})
                # One report per rendering child's log (a child's debug output can appear in its log twice).
                record['child_input']=[]
                for n in logs:
                    found=re.search(r'PS5_PROTON_GRAPHICS_CHILD input result=(\d+) buttons=(0x[0-9a-f]+|0) packet=(\d+) lx=(-?\d+)',(ROOT/'klog'/(args.name+'-'+n)).read_text())
                    if found:record['child_input'].append({'log':n,'result':int(found[1]),'buttons':int(found[2],0),'packet':int(found[3]),'lx':int(found[4])})
                record['child_input_pages']=sum('wine-child: title input page ready' in (ROOT/'klog'/(args.name+'-'+n)).read_text() for n in logs)
                if args.pad_script:
                    record['passed'] &= len(record['child_input'])==4 and all(q['result']==0 and q['buttons']&0x8000 and q['lx']>8000 for q in record['child_input'])
                if args.case=='graphics-broker':
                    debug_lines='\n'.join(re.findall(r'OutputDebugStringA "([^"\n]*)',child_text))
                    record['graphics_metadata_passes']=debug_lines.count('PS5_PROTON_GRAPHICS_CHILD PASS: Windows parent and inherited pipe')
                    record['graphics_teardown_passes']=debug_lines.count('PS5_PROTON_GRAPHICS_CHILD PASS: GPU initialized')
                    record['graphics_descendant_passes']=debug_lines.count('PS5_PROTON_GRAPHICS_CHILD PASS: rendering descendant during GPU lifetime')
                    rendering=[c for c in record['gpu_clients_closed'] if c['frames']]
                    record['passed'] &= 'PS5_PROTON_GRAPHICS_BROKER PASS' in child_text and record['graphics_metadata_passes']==4 and \
                        record['graphics_teardown_passes']==4 and record['graphics_descendant_passes']==2 and \
                        len(rendering)==4 and len({c['pid'] for c in rendering})==4 and all(c['frames']==8 for c in rendering) and \
                        len(record['gpu_display_captures'])==16 and all(c['pixels_passed'] for c in record['gpu_display_captures']) and \
                        len(record['gpu_readbacks'])==4 and all(c['pixels_passed'] for c in record['gpu_readbacks'])
                if args.test_submit_failure:
                    # Rendering is meant to fail here; what must hold is that the title survives the broker's failure.
                    run=json.loads((ROOT/'evidence'/(args.name+'.json')).read_text())
                    record['title_returned']=bool(run.get('returned_to_launcher'))
                    record['passed']=record['title_returned'] and record['broker_wedged']==['test_submit_failure'] and \
                        'gpu-wine stopped=1' in broker_text and not record['post_launch_fault_records'] if 'post_launch_fault_records' in record else False
                ftp.cwd(BASE)
            if args.case=='parent-exit':
                survived=any('PS5_PROTON_PROCESS_SURVIVED_PARENT PASS' in (ROOT/'klog'/(args.name+'-'+name)).read_text() for name in logs)
                record['child_survived_windows_parent']=survived
                record['passed']=record['passed'] and survived
    finally:
        try:
            if idle():
                with console_guard.writing('probe',settings),connect() as ftp:
                    if param_deployed:
                        ftp.storbinary('STOR '+BASE+'sce_sys/param.json',io.BytesIO(original_param))
                        record['param_restored']=download(ftp,BASE+'sce_sys/param.json')==original_param
                    if parent_deployed:
                        ftp.storbinary('STOR '+BASE+'eboot.bin',io.BytesIO(production_image))
                        record['parent_restored']=verified(download(ftp,BASE+'eboot.bin'),production_image,production_elf)
                    restored=True
                    for name,_,_,original,expected in uploads:
                        if name not in deployed:continue
                        ftp.storbinary('STOR '+RUNTIME+name+'.prx',io.BytesIO(original))
                        ftp.storbinary('STOR '+RUNTIME+name+'.prx.elf',io.BytesIO(expected))
                        restored=restored and verified(download(ftp,RUNTIME+name+'.prx'), original, expected)
                        restored=restored and download(ftp,RUNTIME+name+'.prx.elf')==expected
                    record['production_restored']=restored
                    def put_back(path):
                        if path not in production_files:remove(ftp,path);return True
                        ftp.storbinary('STOR '+path,io.BytesIO(production_files[path]))
                        readback=download(ftp,path)
                        return verified(readback,production_files[path],production_file_elfs[path]) if path in production_file_elfs else readback==production_files[path]
                    if args.case=='payload-owner-close':
                        path='/data/ps5proton/prefixes/probe/drive_c/payload-owner-ready'
                        if download(ftp,path,True):remove(ftp,path)
                    if payload_uploaded:record['payload_restored']=put_back(payload_path)
                    if helper_uploaded:record['helper_removed']=put_back(BASE+'wine-child.bin')
                    if probe_uploaded:record['probe_removed']=put_back('/data/ps5proton/prefixes/probe/drive_c/process-probe.exe')
                    if java_uploaded:remove(ftp,BASE+'JavaProcessProbe.class');record['java_removed']=True
                    if graphics_uploaded:
                        record['graphics_removed']=put_back(graphics_path)
                        if download(ftp,capture_path,True):remove(ftp,capture_path)
                        record['graphics_capture_removed']=not download(ftp,capture_path,True)
                    if args.gpu_broker and parent_deployed:
                        # Only synthetic captures created after our empty-path preflight.
                        ftp.cwd(BASE)
                        for n in names(ftp):
                            if n not in old_logs and re.fullmatch(r'gpu-child-\d+-\d+\.ppm',n):remove(ftp,BASE+n)
                        ftp.cwd('/data/ps5proton/logs')
                        for n in names(ftp):
                            if re.fullmatch(r'graphics-broker-\d+\.ppm',n):remove(ftp,'/data/ps5proton/logs/'+n)
                        if download(ftp,BASE+'gpu-broker-results.txt',True):remove(ftp,BASE+'gpu-broker-results.txt')
                    captures_restored=True
                    for path,data in prior_captures.items():
                        ftp.storbinary('STOR '+path,io.BytesIO(data));captures_restored &= download(ftp,path)==data
                    record['prior_captures_restored']=captures_restored
                    for name in record.get('child_logs',[]):remove(ftp,BASE+name)
            else:record['restoration_error']='Console busy; signed production backups retained'
        except BaseException as e:record['restoration_error']=redact(e)
        # What was not put back is not the verified copy: the next launch is refused until a read-back or a deploy.
        if (param_deployed and not record.get('param_restored')) or (parent_deployed and not record.get('parent_restored')) or \
                (helper_uploaded and not record.get('helper_removed')):
            console_guard.record_unverified('title',args.name+': the probe did not put the production title back')
        if (deployed and not record['production_restored']) or (payload_uploaded and not record.get('payload_restored')):
            console_guard.record_unverified('runtime',args.name+': the probe did not put the production runtime back')
        if param_deployed:record['passed']=record['passed'] and record.get('param_restored',False)
        record['passed']=record['passed'] and record['production_restored'] and record['helper_removed'] and \
                         record.get('probe_removed',not probe_uploaded) and record.get('java_removed',not java_uploaded)
        if prior_captures:record['passed']=record['passed'] and record.get('prior_captures_restored',False)
        if args.payload:record['passed']=record['passed'] and record.get('payload_restored',False)
        if owner_manifest:record['passed']=record['passed'] and record.get('parent_restored',False)
        if graphics_uploaded:record['passed']=record['passed'] and record.get('graphics_removed',False) and record.get('graphics_capture_removed',False)
        evidence.write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({k:record.get(k) for k in ('passed','status','production_restored','helper_removed','child_logs','restoration_error')},indent=2))
    return 0 if record['passed'] else 1
if __name__=='__main__':
    try:sys.exit(main())
    except BaseException as e:
        if isinstance(e,SystemExit) and isinstance(e.code,int):raise
        if ACTIVE and ACTIVE.exists():
            record=json.loads(ACTIVE.read_text());record.update(passed=False,error=redact(e));ACTIVE.write_text(json.dumps(record,indent=2)+'\n')
        print('Process probe failed: '+redact(e),file=sys.stderr);sys.exit(1)
