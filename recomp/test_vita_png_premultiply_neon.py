"""Actual ARMv7 execution of the production premultiply TU, OFF versus ON.

Reuses the existing PNG NEON Clang/Unicorn approach and the frozen tables.
No Vita/VM; instruction execution proves bytes/bounds, not target timing.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ,
    UC_HOOK_MEM_WRITE, UC_HOOK_CODE, UC_MEM_READ)
import unicorn.arm_const as ARM
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

ROOT=Path(__file__).resolve().parent
BASE='82c1c16'
BINARY_BASE='f54e737'
LINEAR_BASE='9658c40'
PE_SHA='31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404'
CODE,DATA,TABLE,PARAM,STACK,STOP=0x100000,0x200000,0x500000,0x600000,0x700000,0x800000
GAMMA_TABLE,LINEAR_TABLE=0x30626a40,0x30636a40
SYMBOL='isaac_vita_png_premultiply_rows'

def run(command,source=None):
    result=subprocess.run([str(x) for x in command],input=source,capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{command}:\n{result.stdout.decode(errors="replace")}{result.stderr.decode(errors="replace")}')
    return result.stdout.decode()

def tool(clang,name):
    return clang.parent/(name+('.exe' if clang.suffix=='.exe' else ''))

def compile_arm(clang,work):
    # Only declarations for unused guest machinery: the complete real TU is
    # compiled, then gc-sections retains its pure production rows entry.
    (work/'setjmp.h').write_text('typedef long jmp_buf[32];\n',encoding='ascii')
    (work/'string.h').write_text('#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\n'
        'void *memset(void *,int,size_t);\nint memcmp(const void *,const void *,size_t);\n',encoding='ascii')
    (work/'errno.h').write_text('extern int errno;\n',encoding='ascii')
    flags=[clang,'--target=armv7-none-eabi','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-ffreestanding','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',
        '-mcpu=cortex-a9','-mfpu=neon','-mfloat-abi=softfp','-marm',
        '-DISAAC_VITA_PNG_PREMULTIPLY_NATIVE=1','-DISAAC_VITA_HEAP_RANGE_LEASE=1',
        '-I',work,'-I',ROOT/'runtime']
    source=(ROOT/'runtime/host_vita_png_premultiply.c').read_bytes()
    old=subprocess.check_output(['git','-C',str(ROOT.parent),'show',
        f'{BASE}:recomp/runtime/host_vita_png_premultiply.c'])
    for name,content in (('original-off',old),('current-off',source)):
        run([*flags,'-DISAAC_VITA_PNG_PREMULTIPLY_NEON=0','-x','c','-','-c','-o',work/f'{name}.o'],content)
    assert (work/'original-off.o').read_bytes()==(work/'current-off.o').read_bytes(), 'OFF complete production ARM object changed'
    print('OFF complete production ARM object identity PASS',hashlib.sha256((work/'current-off.o').read_bytes()).hexdigest(),flush=True)
    old_headers=work/'binary-off-base';old_headers.mkdir(exist_ok=True)
    header='recomp/runtime/host_vita_png_premultiply_neon.h'
    (old_headers/Path(header).name).write_bytes(subprocess.check_output(
        ['git','-C',str(ROOT.parent),'show',f'{BINARY_BASE}:{header}']))
    for name,includes in (('base',[ '-I',old_headers]),('current',[])):
        run([flags[0],*includes,*flags[1:],'-DISAAC_VITA_PNG_PREMULTIPLY_NEON=1',
             '-x','c','-','-c','-o',work/f'binary-off-{name}.o'],source)
    assert (work/'binary-off-base.o').read_bytes()==(work/'binary-off-current.o').read_bytes(), 'binary-OFF complete production ARM object changed'
    print('Binary-OFF/uniform-ON complete ARM object identity PASS',hashlib.sha256((work/'binary-off-current.o').read_bytes()).hexdigest(),flush=True)
    linear_headers=work/'linear-off-base';linear_headers.mkdir(exist_ok=True)
    (linear_headers/Path(header).name).write_bytes(subprocess.check_output(
        ['git','-C',str(ROOT.parent),'show',f'{LINEAR_BASE}:{header}']))
    old_source=subprocess.check_output(['git','-C',str(ROOT.parent),'show',
        f'{LINEAR_BASE}:recomp/runtime/host_vita_png_premultiply.c'])
    for name,includes,content in (('base',['-I',linear_headers],old_source),('current',[],source)):
        run([flags[0],*includes,*flags[1:],'-DISAAC_VITA_PNG_PREMULTIPLY_NEON=1',
             '-DISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON=1',
             '-x','c','-','-c','-o',work/f'linear-off-{name}.o'],content)
    assert (work/'linear-off-base.o').read_bytes()==(work/'linear-off-current.o').read_bytes(), 'linear-OFF complete production ARM object changed'
    print('Linear-OFF/binary-ON complete ARM object identity PASS',hashlib.sha256((work/'linear-off-current.o').read_bytes()).hexdigest(),flush=True)
    linker=work/'premultiply.ld'
    linker.write_text('SECTIONS { . = 0x100000; .text : { *(.text*) } '
        '.rodata : { *(.rodata*) } /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n',encoding='ascii')
    machines=[];guards=[]
    for mode,(enabled,binary,linear) in enumerate(((0,0,0),(1,0,0),(1,1,0),(1,1,1),(1,0,1))):
        stem=work/f'premultiply-{mode}'
        definitions=[f'-DISAAC_VITA_PNG_PREMULTIPLY_NEON={enabled}',
                     f'-DISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON={binary}',
                     f'-DISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON={linear}']
        run([*flags,*definitions,'-x','c','-','-c','-o',stem.with_suffix('.o')],source)
        guard=work/f'guest-guard-{mode}.bin'
        run([tool(clang,'llvm-objcopy'),'-O','binary',
            '--only-section=.text.isaac_vita_png_premultiply_guest_try',stem.with_suffix('.o'),guard])
        guards.append(guard.read_bytes());assert guards[-1]
        macros=run([*flags,*definitions,'-dM','-E','-x','c','-'],source)
        assert f'#define ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE {enabled}' in macros
        (work/f'macros-{mode}.txt').write_text(macros,encoding='ascii')
        run([tool(clang,'ld.lld'),'--gc-sections','-T',linker,'--entry='+SYMBOL,
            stem.with_suffix('.o'),'-o',stem.with_suffix('.elf')])
        symbols=run([tool(clang,'llvm-nm'),stem.with_suffix('.elf')])
        entry=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' '+SYMBOL))
        run([tool(clang,'llvm-objcopy'),'-O','binary','--only-section=.text','--only-section=.rodata',
            stem.with_suffix('.elf'),stem.with_suffix('.bin')])
        code=stem.with_suffix('.bin').read_bytes()
        disassembly=list(Cs(CS_ARCH_ARM,CS_MODE_ARM).disasm(code,CODE))
        (work/f'assembly-{mode}.txt').write_text('\n'.join(f'{i.address:08x}: {i.mnemonic} {i.op_str}'
            for i in disassembly)+'\n',encoding='ascii')
        neon_loads={i.address for i in disassembly if i.mnemonic.startswith('vld4')}
        neon_stores={i.address for i in disassembly if i.mnemonic.startswith('vst1')}
        binary_stores={i.address for i in disassembly if i.mnemonic.startswith('vst4')}
        assert bool(neon_loads)==bool(enabled)
        assert bool(binary_stores)==bool(binary)
        linear_ops={i.address for i in disassembly if i.mnemonic in ('vmull.u8','vmlal.u8')}
        assert bool(linear_ops)==bool(linear)
        machines.append(Machine(code,entry,neon_loads,neon_stores,binary,binary_stores,
                                linear=linear,linear_ops=linear_ops))
        print(f'ARM mode {mode}: linked pure rows text/constants {len(code)} bytes; available={enabled} binary={binary} linear={linear}',flush=True)
    assert all(g==guards[0] for g in guards), 'guest admission/lease/CPU-continuation machine code changed'
    print('OFF/ON guest guard machine-code section identical',hashlib.sha256(guards[0]).hexdigest(),flush=True)
    return machines

class Machine:
    def __init__(self,code,entry,neon_loads,neon_stores,binary=False,binary_stores=frozenset(),direct=False,
                 linear=False,linear_ops=frozenset(),thumb=False):
        self.thumb=thumb
        self.uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB if thumb else UC_MODE_ARM)
        if hasattr(self.uc,'ctl_set_cpu_model'):
            self.uc.ctl_set_cpu_model(ARM.UC_CPU_ARM_CORTEX_A9)
        for base,size in ((CODE,0x20000),(DATA,0x200000),(TABLE,0x10000),(PARAM,0x1000),(STACK,0x10000),(STOP,0x1000),
                          (GAMMA_TABLE&~4095,0x21000)):
            self.uc.mem_map(base,size)
        self.uc.mem_write(CODE,code)
        self.uc.reg_write(ARM.UC_ARM_REG_C1_C0_2,0xf<<20)
        self.uc.reg_write(ARM.UC_ARM_REG_FPEXC,0x40000000)
        self.entry=entry;self.neon_loads=neon_loads;self.neon_stores=neon_stores
        self.executed_neon_loads=self.executed_neon_stores=0
        self.binary=binary;self.binary_stores=binary_stores;self.executed_binary_stores=0
        self.linear=linear;self.linear_ops=linear_ops;self.executed_linear_ops=0
        self.direct=direct
        self.uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,self.access)
        self.uc.hook_add(UC_HOOK_CODE,self.instruction)

    def instruction(self,uc,address,size,user):
        if address in self.neon_loads:self.executed_neon_loads+=1
        if address in self.neon_stores:self.executed_neon_stores+=1
        if address in self.binary_stores:self.executed_binary_stores+=1
        if address in self.linear_ops:self.executed_linear_ops+=1

    def access(self,uc,access,address,size,value,user):
        if STACK<=address and address+size<=STACK+0x10000:return
        if access==UC_MEM_READ and CODE<=address and address+size<=CODE+0x20000:return
        if access==UC_MEM_READ and PARAM<=address and address+size<=PARAM+24:return
        if access==UC_MEM_READ and self.table_address<=address and address+size<=self.table_address+65536:
            assert not self.direct, 'block helper read a table'
            assert self.has_partial, 'table read for exclusively zero/opaque input'
            self.table_reads+=1;return
        offset=address-self.pointer
        if offset>=0 and self.stride and offset//self.stride<self.rows and offset%self.stride+size<=self.width*4:
            if access!=UC_MEM_READ:
                assert not (self.direct and self.has_partial), 'rejected block helper wrote before scalar fallback'
                assert not self.all_opaque, 'opaque input was written'
                row=offset//self.stride;column=offset%self.stride
                touched=self.alpha[row*self.width+column//4:row*self.width+(column+size-1)//4+1]
                if 255 in touched:
                    block_start=(column//64)*16
                    block=self.alpha[row*self.width+block_start:row*self.width+block_start+16]
                    assert block_start+16<=self.width and len(block)==16
                    accepted_binary=self.binary and set(block)=={0,255}
                    accepted_linear=self.linear and self.table_address==LINEAR_TABLE and len(set(block))>1
                    assert accepted_binary or accepted_linear, 'opaque write outside accepted full block'
                    assert (column+size-1)//64==column//64, 'binary store crossed block boundary'
                self.pixel_writes+=1
            return
        raise AssertionError(f'premultiply access outside logical row: {access} {address:x}+{size}; '
            f'base={self.pointer:x} {self.width}x{self.rows} stride={self.stride}')

    def call(self,raw,width,rows,stride,alignment,table,table_address=TABLE):
        self.pointer=DATA+0x100+alignment;self.width=width;self.rows=rows;self.stride=stride
        self.table_address=table_address
        alpha=[raw[y*stride+x*4+3] for y in range(rows) for x in range(width)]
        self.alpha=alpha
        self.has_partial=any(a not in (0,255) for a in alpha)
        self.all_opaque=all(a==255 for a in alpha)
        self.table_reads=self.pixel_writes=0
        self.uc.mem_write(self.pointer-32,b'\xa7'*32+raw+b'\xb9'*32)
        self.uc.mem_write(table_address,table)
        params=struct.pack('<6I',self.pointer,len(raw),width,rows,stride,table_address)
        self.uc.mem_write(PARAM,params+b'\xc1'*32)
        registers=[getattr(ARM,f'UC_ARM_REG_R{i}') for i in range(4,12)]
        values=[0xabc000+i for i in range(8)]
        doubles=[getattr(ARM,f'UC_ARM_REG_D{i}') for i in range(8,16)]
        doublevalues=[0xaabc000000000000+i for i in range(8)]
        for reg,value in zip(registers+doubles,values+doublevalues):self.uc.reg_write(reg,value)
        self.uc.reg_write(ARM.UC_ARM_REG_SP,STACK+0x8000)
        self.uc.reg_write(ARM.UC_ARM_REG_LR,STOP|int(self.thumb))
        self.uc.reg_write(ARM.UC_ARM_REG_R0,self.pointer if self.direct else PARAM)
        self.uc.emu_start(self.entry,STOP,count=30000000)
        assert self.uc.reg_read(ARM.UC_ARM_REG_PC)==STOP,'did not return'
        if self.direct:
            assert self.uc.reg_read(ARM.UC_ARM_REG_R0)==int(not self.has_partial)
        assert self.uc.reg_read(ARM.UC_ARM_REG_SP)==STACK+0x8000
        assert [self.uc.reg_read(r) for r in registers+doubles]==values+doublevalues,'callee-saved register changed'
        assert bytes(self.uc.mem_read(PARAM,56))==params+b'\xc1'*32
        result=bytes(self.uc.mem_read(self.pointer-32,len(raw)+64))
        assert result[:32]==b'\xa7'*32 and result[-32:]==b'\xb9'*32
        assert bytes(self.uc.mem_read(table_address,65536))==table
        return result[32:-32]


def direct_blocks(clang,work,table):
    # Compile the actual inline block helper, not a copied implementation, so
    # rejected near-binary input can prove NO writes before scalar fallback.
    source='#include "host_vita_png_premultiply_neon.h"\nint block_test(uint8_t *p) { return isaac_png_premultiply_uniform16(p); }\n'
    stem=work/'binary-direct'
    run([clang,'--target=armv7-none-eabi','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-ffreestanding','-ffunction-sections','-fdata-sections','-mcpu=cortex-a9',
        '-mfpu=neon','-mfloat-abi=softfp','-marm','-DISAAC_VITA_PNG_PREMULTIPLY_NATIVE=1',
        '-DISAAC_VITA_PNG_PREMULTIPLY_NEON=1','-DISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON=1',
        '-I',ROOT/'runtime','-x','c','-','-c','-o',stem.with_suffix('.o')],source.encode())
    run([tool(clang,'ld.lld'),'--gc-sections','-T',work/'premultiply.ld','--entry=block_test',
        stem.with_suffix('.o'),'-o',stem.with_suffix('.elf')])
    symbols=run([tool(clang,'llvm-nm'),stem.with_suffix('.elf')])
    entry=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' block_test'))
    run([tool(clang,'llvm-objcopy'),'-O','binary','--only-section=.text','--only-section=.rodata',
        stem.with_suffix('.elf'),stem.with_suffix('.bin')])
    machine=Machine(stem.with_suffix('.bin').read_bytes(),entry,set(),set(),binary=True,direct=True)
    count=0
    for alpha in range(256):
        for lane in range(16):
            raw=bytearray()
            for x in range(16):
                raw+=bytes(((x*13)&255,0xa1,0x5c,alpha if x==lane else (255 if x&1 else 0)))
            expected=reference(raw,16,1,64,table) if alpha in (0,255) else raw
            assert machine.call(raw,16,1,64,lane,table)==expected
            count+=1
    for alpha in (0,255):
        raw=bytes((0x91,0x72,0x53,alpha))*16
        assert machine.call(raw,16,1,64,7,table)==reference(raw,16,1,64,table)
        count+=1
    print(f'Actual ARM binary block helper PASS {count} calls; partial-alpha rejection makes no write/table access',flush=True)

def reference(raw,width,rows,stride,table):
    out=bytearray(raw)
    for y in range(rows):
        for x in range(width):
            i=y*stride+x*4;a=out[i+3]
            if a==255:continue
            for c in range(3):out[i+c]=0 if a==0 else table[(a<<8)+out[i+c]]
    return bytes(out)

def cases():
    rng=random.Random(0x1600a1fa)
    for width in range(49):
        for rows in (0,1,3):
            for padding in (0,5):
                for kind in ('zero','opaque','binary','mixed'):
                    stride=width*4+padding;raw=bytearray(b'\x6d'*(max(rows,1)*stride+17))
                    for y in range(rows):
                        for x in range(width):
                            i=y*stride+x*4
                            a=0 if kind=='zero' else 255 if kind=='opaque' else (255 if x&1 else 0) if kind=='binary' else rng.randrange(256)
                            raw[i:i+4]=bytes((rng.randrange(256),rng.randrange(256),rng.randrange(256),a))
                    yield raw,width,rows,stride,(width+rows+padding)&15
    for tail in range(16):
        width=96+tail;rows=3;stride=width*4+13;raw=bytearray(b'\x6d'*(rows*stride+17))
        for y in range(rows):
            for x in range(width):
                a=(0,255,(x*13+y)&255)[(x//16+y)%3]
                i=y*stride+x*4;raw[i:i+4]=bytes((x&255,(x+y)&255,255-(x&255),a))
        yield raw,width,rows,stride,tail
    # Near-binary blocks: each possible alpha at each lane, with both binary
    # endpoint values elsewhere. Keep tails and following logical rows too.
    for alpha in range(256):
        for lane in range(16):
            width=33;rows=2;stride=width*4+7;raw=bytearray(b'\x6d'*(rows*stride+17))
            for y in range(rows):
                for x in range(width):
                    a=alpha if x==lane else (255 if (x+y)&1 else 0)
                    i=y*stride+x*4;raw[i:i+4]=bytes(((x*31)&255,(y*71+x)&255,0xa7,a))
            yield raw,width,rows,stride,lane
    # Both frozen tables see every (alpha, red) pair; the other colors are
    # permutations so they too span all color inputs for every alpha.
    width=rows=256;stride=width*4+13;raw=bytearray(b'\x6d'*(rows*stride+17))
    for y in range(rows):
        for x in range(width):
            i=y*stride+x*4;raw[i:i+4]=bytes((x,(x+71)&255,255-x,y))
    yield raw,width,rows,stride,7

def host_guards(args):
    if os.name=='nt':
        from setuptools.msvc import msvc14_get_vc_env
        os.environ.update({k.upper():v for k,v in msvc14_get_vc_env('x64').items()})
    import test_vita_png_premultiply as existing
    original_run=existing.run
    output=[]
    def with_option(cmd):
        if str(cmd[0]).endswith('clang.exe') or str(cmd[0]).endswith('/clang'):
            cmd=[*cmd,'-DISAAC_VITA_PNG_PREMULTIPLY_NEON=1',
                 '-DISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON=1',
                 '-DISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON=1']
        result=original_run(cmd);output.append(result)
        return result
    existing.run=with_option
    sys.argv=['test_vita_png_premultiply.py','--pe',str(args.pe),'--out',str(args.out/'host-guards'),'--cc',str(args.clang)]
    assert existing.main()==0
    (args.out/'host-guards/result.log').write_text(''.join(output),encoding='utf-8')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--clang',type=Path,required=True)
    ap.add_argument('--pe',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--host',action='store_true')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    assert hashlib.sha256(args.pe.read_bytes()).hexdigest()==PE_SHA
    os.environ['REPENTOGXM_PE']=str(args.pe)
    import gen_all as G
    from image import Image,DEFAULT_BASE
    image=Image(str(args.pe),DEFAULT_BASE)
    tables=[]
    for rva,digest in G.VITA_PNG_PREMULTIPLY_TABLES:
        data=image.code_at(rva,65536);assert hashlib.sha256(data).hexdigest()==digest;tables.append(data)
        (args.out/f'table-{rva:08x}.bin').write_bytes(data)
    for a in range(256):
        for c in range(256):
            t=a*c+128
            assert tables[1][a*256+c]==(a*c+127)//255==(t+(t>>8))>>8
    print('Frozen linear table: exhaustive 65536-entry scalar/NEON integer identity PASS',flush=True)
    machines=compile_arm(args.clang,args.out)
    direct_blocks(args.clang,args.out,tables[0])
    count=0
    for raw,width,rows,stride,alignment in cases():
        for index,table in enumerate(tables):
            expected=reference(raw,width,rows,stride,table)
            for machine in machines:
                address=(GAMMA_TABLE,LINEAR_TABLE)[index] if machine.linear else TABLE
                before=machine.executed_linear_ops
                assert machine.call(raw,width,rows,stride,alignment,table,address)==expected,(count,width,rows,stride,alignment)
                if index==0:assert machine.executed_linear_ops==before, 'gamma took the linear path'
                if machine.linear:
                    # Identical linear bytes at a noncanonical pointer must use
                    # the supplied table, not infer a formula from its contents.
                    before=machine.executed_linear_ops
                    assert machine.call(raw,width,rows,stride,alignment,table)==expected
                    assert machine.executed_linear_ops==before, 'noncanonical table took the linear path'
            count+=1
        if count%512==0:print(f'ARM comparisons completed {count}',flush=True)
    assert machines[1].executed_neon_loads and machines[1].executed_neon_stores,'NEON was not executed'
    assert machines[2].executed_binary_stores, 'binary NEON store was not executed'
    assert machines[3].executed_linear_ops, 'linear NEON arithmetic was not executed'
    raw=bytes((13,119,241,127))*32
    assert machines[3].call(raw,32,1,128,3,tables[1],LINEAR_TABLE)==reference(raw,32,1,128,tables[1])
    assert machines[3].table_reads==0, 'full linear blocks still read the lookup table'
    assert not machines[0].executed_neon_loads and not machines[0].executed_neon_stores
    summary=(f'Actual Cortex-A9 ARM PASS: {count} comparisons per mode (scalar/uniform/binary/linear with and without binary); both frozen tables; '
        f'NEON loads={machines[1].executed_neon_loads} stores={machines[1].executed_neon_stores}; '
        f'binary-mode stores={machines[2].executed_binary_stores}; '
        f'linear-mode multiply ops={machines[3].executed_linear_ops}; '
        'row/padding/tail/table access hooks, guard bytes and callee-saved registers\n')
    (args.out/'result.txt').write_text(summary,encoding='utf-8');print(summary,flush=True)
    if args.host:host_guards(args)
    return 0

if __name__=='__main__':raise SystemExit(main())
