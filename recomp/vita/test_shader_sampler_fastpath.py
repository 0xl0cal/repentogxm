#!/usr/bin/env python3
"""Regenerate and execute frozen sampler bodies against native typed wrappers.

No full corpus/emitter changes: link-wrap only an exclusively vtable-dispatched
root. OFF backend object/token identity and the actual CMake owner/link block
are checked. This is not a driver/GPU/frame-rate measurement.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'recomp'))
import gen_all
from image import Image
GUARD='ISAAC_VITA_SHADER_SAMPLER_FASTPATH'
BASE='5a1b0d25e0eedecd8086c80a999550af0fc97f9a'
PE_SHA='31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404'


def run(argv,**kw):
    p=subprocess.run(list(map(str,argv)),capture_output=True,text=True,**kw)
    if p.returncode:raise RuntimeError(f'{argv}\n{p.stdout}{p.stderr}')
    return p.stdout


def strip(text):
    out=[];depth=0
    for line in text.splitlines(keepends=True):
        if not depth and line.strip()==f'#if defined({GUARD}) && {GUARD}':depth=1
        elif depth:
            if re.match(r'\s*#\s*if',line):depth+=1
            elif re.match(r'\s*#\s*endif',line):depth-=1
        else:out.append(line)
    assert not depth
    return re.findall(r'"(?:\\.|[^"\\])*"|\w+|[^\s]',re.sub(r'/\*.*?\*/|//[^\n]*','',''.join(out),flags=re.S))


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--pe',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'build/sampler-fastpath')
    parser.add_argument('--guard',type=int,choices=(0,1),default=0)
    parser.add_argument('--baseline',default=BASE,
                        help='OFF identity parent (use integration parent when cherry-picking only this candidate)')
    a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    assert hashlib.sha256(a.pe.read_bytes()).hexdigest()==PE_SHA
    pin=Image(str(a.pe),0x400000);image=Image(str(a.pe),0x98000000)
    raw=pin.code_at(0x567430,0x56)
    assert hashlib.sha256(raw).hexdigest()=='89109c84f59f42793f533e269f4fe47bf2e7d539b0ed34fd997eb96a5ba31e07'
    assert struct.unpack('<9I',pin.code_at(0x75e0a8,36))[7]==0x967430
    assert not gen_all._vita_png_decode_profile_rel32_xrefs(pin,0x567430)
    refs=[r for r in sorted(pin.reloc_rvas) if int.from_bytes(pin.code_at(r,4),'little')==0x967430]
    assert refs==[0x75e0c4],refs
    import pefile
    pe=pefile.PE(str(a.pe));assert pe.OPTIONAL_HEADER.SizeOfImage==0x85f000
    text=gen_all._translate_function(image,{'rva':0x567430},None,{},pin_img=pin)['text']
    # Four sites, 31 real instructions, correct return and deepest stack shape.
    assert '31 insns' in text and text.count('guest_call(c, _target)')==4
    assert 'ret 0xc' in text and 'GUEST_FLAGS_FLUSH' not in text
    for slot in (0x7c293c,0x7c2978,0x7bf9bc,0x7bf96c):
        assert f'call dword ptr [0x{0x98000000+slot:x}]' in text
    (out/'sampler-fresh.c').write_text('#include "guest.h"\n'+text)
    print('PASS owned PE/body/31 instructions/four GL slots/exclusive vtable root; freshly generated ORIGINAL body')
    print(run([sys.executable,ROOT/'recomp/gl_surface.py','--write'],cwd=ROOT).strip())
    print(run([sys.executable,ROOT/'recomp/vita/test_vita_attrib_boundaries.py','--output',out],cwd=ROOT).strip())
    native=(ROOT/'recomp/runtime/gl_vita_backend_oracle.c').read_text()
    native=native.replace('int main(', 'int sampler_unused_main(',1)
    hooks={
        'oracle_glActiveTexture':'sampler_hook(1u,(uint32_t)texture,0u,0u);',
        'oracle_glBindTexture':'sampler_hook(2u,(uint32_t)target,(uint32_t)texture,0u);',
        'oracle_glGetUniformLocation':'sampler_hook(3u,(uint32_t)program,(uint32_t)(uintptr_t)name,0u);',
        'oracle_glUniform1i':'sampler_hook(4u,(uint32_t)location,(uint32_t)value,0u);',
    }
    for name,hook in hooks.items():
        start=native.index(name+'(');opening=native.index('{',start)
        native=native[:opening+1]+'\n'+hook+native[opening+1:]
    assert native.count('return s_native_uniform_location;')==1
    native=native.replace('return s_native_uniform_location;',
        'return sampler_mode ? sampler_lookup(program,name) : s_native_uniform_location;',1)
    (out/'sampler_native_fixture.inc').write_text(native)
    from setuptools.msvc import msvc14_get_vc_env
    vc=msvc14_get_vc_env('x86');env={k:v for k,v in os.environ.items() if k.lower() not in vc};env.update(vc)
    cc=shutil.which('cl',path=vc['path'])
    common=[cc,'/nologo','/std:c11','/O2','/Gy','/W4','/WX','/wd4310','/wd4505','/D_CRT_SECURE_NO_WARNINGS',
            '/DGUEST_IMAGE_BASE=0x98000000u','/DGUEST_STACK_REQUIRED=1','/DGUEST_GENERATED_STACK_GUARD='+str(a.guard),
            '/DGUEST_FLAGS_LOCAL=1','/DGUEST_COVERAGE_HOOKS=0','/I'+str(out),'/I'+str(ROOT/'recomp/runtime')]
    objects=[]
    for mode,gpr,symbol in [('real',1,'__real_sub_00567430'),('ref',1,'orig_sub_00567430'),
                            ('mem-real',0,'mem_real_sub_00567430'),('mem-ref',0,'orig_mem_sub_00567430')]:
        obj=out/(mode+'.obj')
        # The unedited generated declaration has a dead _fl in this root.
        print(run([*common,'/wd4189','/DGUEST_GPR_LOCAL='+str(gpr),'/Dsub_00567430='+symbol,
                   '/c',out/'sampler-fresh.c','/Fo:'+str(obj)],env=env,cwd=out).strip());objects.append(obj)
    helper=ROOT/'recomp/runtime/host_vita_shader_sampler_fastpath.c'
    helper_fixture=out/'sampler-helper-fixture.c'
    helper_fixture.write_text(helper.read_text()+'\nint oracle_sampler_probe(CPU *c) { return sampler_try(c); }\n')
    flags=['/D'+GUARD+'=1','/DISAAC_GL_VITA_BACKEND_ORACLE=1','/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1',
           '/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1','/DISAAC_VITA_PHASE_PROFILE=1']
    for mode in ('real','mem-real'):
        obj=out/(mode+'-helper.obj');extra=[] if mode=='real' else [
            '/D__wrap_sub_00567430=mem_wrap_sub_00567430','/D__real_sub_00567430=mem_real_sub_00567430']
        source=helper_fixture if mode=='real' else helper
        print(run([*common,*flags,*extra,'/c',source,'/Fo:'+str(obj)],env=env,cwd=out).strip());objects.append(obj)
    # Actual checked stack implementation; only unused dispatch/fatal owners
    # get test symbol names, so the native fixture can own its GL call sink.
    obj=out/'guest-stack-runtime.obj'
    runtime=(ROOT/'recomp/runtime/guest.c').read_text()
    signature='void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)'
    assert runtime.count(signature)==1
    runtime=runtime.replace(signature,signature.replace('guest_fault','sampler_unused_fault_body'),1)
    (out/'guest-stack-runtime.c').write_text(runtime)
    runtime_flags=[v for v in common if not v.startswith('/DGUEST_GENERATED_STACK_GUARD=')]
    print(run([*runtime_flags,'/DGUEST_GENERATED_STACK_GUARD=1',*flags,
               '/Dguest_call=sampler_unused_guest_call',
               '/Dguest_gl_dynamic_first_holds=sampler_unused_dynamic_first_holds',
               '/c',out/'guest-stack-runtime.c',
               '/Fo:'+str(obj)],env=env,cwd=out).strip());objects.append(obj)
    # Runtime fixture + ORIGINAL bodies execute; sinks are native API mocks,
    # not mock-up rewritten bodies. Repeated states include reset and mutation.
    for cache in (False,True):
        exe=out/('sampler-cached.exe' if cache else 'sampler.exe')
        cache_flags=['/DISAAC_VITA_GL_TYPED_STATE_CACHE=1','/DISAAC_VITA_GL_REDUNDANCY_CACHE=1',
                     '/DISAAC_VITA_GL_LOCATION_CACHE=1','/DISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE=1'] if cache else []
        print(run([*common,*flags,*cache_flags,ROOT/'recomp/vita/host_tests/shader_sampler_oracle.c',
                   ROOT/'recomp/runtime/gl_bridge.c',ROOT/'recomp/runtime/gl_vita_backend.c',*objects,
                   '/Fe:'+str(exe),'/Fo:'+str(out)+'\\','/link','/LARGEADDRESSAWARE','/OPT:REF'],env=env,cwd=out).strip())
        print(('CACHED ' if cache else 'PLAIN ')+run([exe],cwd=out).strip())
    # The default-OFF candidate changes NO generated unit, bridge or shader;
    # complete backend native object comparison proves its one guarded addition.
    current=(ROOT/'recomp/runtime/gl_vita_backend.c').read_text()
    # The base leg reads the parent from git history; a checkout without that
    # commit (the public snapshot) skips only this leg.
    have_base=subprocess.run(['git','cat-file','-e',a.baseline+'^{commit}'],cwd=ROOT,
                             capture_output=True).returncode==0
    old=None
    if have_base:
        old=run(['git','show',a.baseline+':recomp/runtime/gl_vita_backend.c'],cwd=ROOT)
        assert strip(current)==strip(old)
    else:
        print('SKIP base leg: commit '+a.baseline+" is not in this checkout's history; off == zero still compared")
    whole=out/'whole';whole.mkdir(exist_ok=True);equal=[]
    for mode in (('base','off','zero','on') if have_base else ('off','zero','on')):
        src=whole/'gl_vita_backend.c';src.write_text(old if mode=='base' else current)
        obj=whole/'gl_vita_backend.obj'
        f=['/DISAAC_GL_VITA_BACKEND_ORACLE=1']
        if mode=='zero':f+=['/D'+GUARD+'=0']
        if mode=='on':f+=['/D'+GUARD+'=1']
        print(run([*common,*f,'/c',src,'/Fo:'+str(obj)],env=env,cwd=whole).strip())
        data=obj.read_bytes();(out/(mode+'-whole.obj')).write_bytes(data)
        equal.append(data[:4]+b'\0'*4+data[8:])
    assert len(set(equal[:3 if have_base else 2]))==1
    print('PASS complete x86 OFF/zero backend COFF identity (timestamp only), baseline='+(a.baseline if have_base else 'skipped')+'; ON native-wrapper matcher /W4 /WX')
    cmake=(ROOT/'recomp/vita/CMakeLists.txt').read_text()
    assert re.search(r'option\(ISAAC_VITA_SHADER_SAMPLER_FASTPATH\s+"[^"]+" OFF\)',cmake)
    begin=cmake.index('  if(ISAAC_VITA_SHADER_SAMPLER_FASTPATH)')
    end=cmake.index('  if(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)',begin)
    block=cmake[begin:end]
    cmake_exe=shutil.which('cmake')
    for on in (0,1):
        script=out/f'cmake-{on}.cmake'
        script.write_text(f'set(ISAAC_VITA_SHADER_SAMPLER_FASTPATH {on})\nset(ISAAC_RUNTIME runtime)\n'
                          'function(set_property)\nendfunction()\nfunction(target_link_options)\nset(LINKED ${ARGN} PARENT_SCOPE)\nendfunction()\n'+block+
                          f'if({on})\nif(NOT ISAAC_VITA_RUNTIME_SOURCES STREQUAL "runtime/host_vita_shader_sampler_fastpath.c")\nmessage(FATAL_ERROR "source owner")\nendif()\nif(NOT LINKED STREQUAL "isaac_first_arm_fault;PRIVATE;-Wl,--wrap=sub_00567430")\nmessage(FATAL_ERROR "link owner")\nendif()\nelse()\nif(ISAAC_VITA_RUNTIME_SOURCES OR LINKED)\nmessage(FATAL_ERROR "OFF effects")\nendif()\nendif()\n')
        print(run([cmake_exe,'-P',script],cwd=out).strip())
    print('PASS actual CMake source/link block OFF no effects, ON exact sampler owner/wrap; emitter and generated tracked inputs unchanged')
    print('SKIP ARM/linker guest-table slot verification, SDK/GXM failure/output, device CPU/FPS gain; no shader arithmetic changed')


if __name__=='__main__':main()
