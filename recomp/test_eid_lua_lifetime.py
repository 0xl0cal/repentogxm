#!/usr/bin/env python3
"""Fresh original J835 temporary-string lifecycle, real Lua/bridge/heap.
No runtime patch, broad census, GPU or device action. Public LuaRef role clue
is factual only; function bodies and constants come from our pinned PE.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys
import build_one as B
from image import Image
import trans as T
import test_vita_lua_runtime as R

ROOT=Path(__file__).resolve().parent
RT=ROOT/"runtime"
PE_HASH="31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
CFG={
    0x445170:(96,"26e2c4ebc229021322a086c652ffefbf579cd33a9235b69197f15ed7dd7b7b9a"),
    0x3fa3b0:(20,"7a2ecd4b9196ccbf2cf6d6ea4d862d6aa35030a3704cb6ceba1713568ab274ae"),
    0x79f0:(53,"4f251ac95624f4c32a0662c7ac117d63538b1b41fd09d8cf6f64a8e0ddbff20d"),
    0x5eb08e:(7,"41def712c2bdb093e51584f334f36ec0f4a476b19ed76659bfaab197bd3109b9"),
    0x5eace5:(3,"0a45200300a43da2df8797d133d0b36d0b3856dbe150eb599a8fc798009d43f2"),
    0x443b10:(11,"800ff775937e45f349be909e1fb007368852de8ac81b7023fd6c959aff58c1f3"),
    0x7be0:(27,"9b887d67d60b3961d15caed964527458d3469d366b9bbcec1f9c134825cc75e6"),
}
def verify_eh(pin:Image)->None:
    # Own J835 data, independently decoded. External role clue only:
    # chiikabu/the-browsing-of-isaac @35f55de6, lua_engine_pure_helpers.h.
    # No foreign implementation/offsets copied. These are NOT general unwind
    # execution rules: the oracle explicitly invokes the pinned action entries.
    u=lambda r:struct.unpack_from("<I",pin.mem,r)[0]
    for owner,handler,info,table,actions in (
        (0x433d70,0x5fc3a8,0x781c48,0x781c6c,
         (0x5fc370,0x5fc378,0x5fc383,0x5fc38b,0x5fc393,0x5fc39b)),
        (0x433f70,0x5fc41b,0x781c9c,0x781cc0,
         (0x5fc3e0,0x5fc3e8,0x5fc3f3,0x5fc3fb,0x5fc403,0x5fc40b)),
    ):
        require(pin.code_at(owner,10)==b"\x55\x8b\xec\x6a\xff\x68"+struct.pack("<I",pin.va(handler)),
                f"EH owner prologue drift {owner:x}")
        handler_ins,handler_order,_handler_indirect=T.decode(pin,handler)
        loads=[r for r in handler_order if handler_ins[r].mnemonic=="mov" and
               len(handler_ins[r].operands)==2 and handler_ins[r].operands[0].type==T.cx.X86_OP_REG and
               handler_ins[r].operands[0].reg==T.cx.X86_REG_EAX and
               handler_ins[r].operands[1].type==T.cx.X86_OP_IMM and
               handler_ins[r].operands[1].imm==pin.va(info)]
        require(len(loads)==1,"EH handler must load its OWN FuncInfo exactly once")
        after=handler_ins[loads[0]+handler_ins[loads[0]].size]
        require(after.mnemonic=="jmp" and after.operands[0].imm==0x5ec134,"EH handler dispatch drift")
        require(u(info)==0x19930522 and u(info+4)==6 and u(info+8)==pin.va(table),"EH FuncInfo drift")
        entries=[struct.unpack_from("<iI",pin.mem,table+i*8) for i in range(6)]
        require(tuple(a-pin.base for _,a in entries)==actions and
                tuple(s for s,_ in entries)==(-1,0,1,-1,3,3),"EH unwind map drift")
        for action,disp in zip((actions[3],actions[5]),
                              (0x34,0x80) if owner==0x433d70 else (0x38,0x8c)):
            ins,order,_indirect=T.decode(pin,action)
            require(ins[action].mnemonic=="lea" and ins[action].operands[1].mem.disp==-disp,
                    "own scope local displacement drift")
            branch=ins[action+ins[action].size]
            require(branch.mnemonic=="jmp" and branch.operands[0].imm==0x443b10,"scope tail target drift")
        print(f"EH source owner={owner:08x} handler={handler:08x} info={info:08x} map={table:08x} PASS")
def require(ok:bool,message:str)->None:
    if not ok: raise AssertionError(message)
def run(cmd:str|list[str],cwd:Path)->str:
    p=subprocess.run(cmd,cwd=cwd,shell=isinstance(cmd,str),text=True,
                     stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    require(p.returncode==0,f"command failed {p.returncode}: {cmd}\n{p.stdout}")
    return p.stdout
def main()->int:
    p=argparse.ArgumentParser()
    p.add_argument("--pe",type=Path,required=True)
    p.add_argument("--lua-source",type=Path,required=True)
    p.add_argument("--work",type=Path,required=True)
    args=p.parse_args();work=args.work.resolve();work.mkdir(parents=True,exist_ok=True)
    require(hashlib.sha256(args.pe.read_bytes()).hexdigest()==PE_HASH,"wrong frozen PE")
    pin=Image(str(args.pe),0x400000)
    verify_eh(pin)
    for root,(count,digest) in CFG.items():
        ins,order,indirect=T.decode(pin,root)
        got=hashlib.sha256(b"".join(struct.pack("<I",r)+bytes(ins[r].bytes) for r in order)).hexdigest()
        expected_indirect=[0x5ec176] if root==0x5eace5 else []
        require(indirect==expected_indirect and len(order)==count and got==digest,f"original body changed {root:x}")
    import pefile
    pe=pefile.PE(str(args.pe))
    imports={v.address-pe.OPTIONAL_HEADER.ImageBase:(d.dll.decode(),v.name.decode())
             for d in pe.DIRECTORY_ENTRY_IMPORT for v in d.imports if v.name}
    require(imports[0x606488]==("VCRUNTIME140.dll","memcpy"),"memcpy IAT identity changed")
    require(imports[0x606248]==("Lua5.3.3r.dll","luaL_unref"),"unref IAT identity changed")
    require(hashlib.sha256(pin.code_at(0x443b10,32)).hexdigest()==
            "092799eaac0824494b6296b26ee683c2206490d54be830e5732fdb6239b04b3c","LuaRef complete body drift")
    image=Image(str(args.pe),0x98000000)
    private=work/"fresh-image.bin";private.write_bytes(image.mem)
    generated=[]
    roots=[0x445170,0x3fa3b0,0x79f0,0x7de0,0x5eacf8,0x1f50,0x1f30,0x5e1910,
           0x5eb08e,0x5ebd94,0x5ec176,0x5ec17c,0x5ec18e,0x5ec158,0x5ec14c,
           0x3f8e30,0x3f8c80,0x443b10,0x7be0,0x5fc38b,0x5fc39b,0x5fc3fb,0x5fc40b]
    for root in roots:
        name=f"sub_{root:08x}"
        if root in (0x3f8e30,0x3f8c80):name="__real_"+name
        em,ins,order,body,unsupported,indirect,_members=B.translate(image,root,name)
        require(not unsupported,f"generation failed {root:x}: {unsupported}")
        dest=work/(name+".c");B.write_c(dest,name,em,body,order,root);generated.append(dest)
    source=R.discover_source(args.lua_source)
    lua=[x for x in R.validate_source(source,R.records()) if x.name not in ("lua.c","luac.c")]
    vc=R.find_vcvars();prefix=f"call {R.quote(vc)} x86 >nul && "
    common=["cl","/nologo","/std:c11","/O2","/D_CRT_SECURE_NO_WARNINGS",
            f"/I{R.quote(source/'src')}",f"/I{R.quote(RT)}"]
    run(prefix+" ".join([*common,"/W3","/c",*[R.quote(x) for x in lua]]),work)
    defs=["/DGUEST_IMAGE_BASE=0x98000000u","/DGUEST_GPR_LOCAL=1","/DGUEST_FLAGS_LOCAL=1",
          "/DISAAC_VITA_HEAP_TESTING=1","/DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1",
          "/DISAAC_VITA_HEAP_RANGE_LEASE=1"]
    run(prefix+" ".join([*common,"/W3",*defs,"/DGUEST_GENERATED_STACK_GUARD=0","/c",
                         *[R.quote(x) for x in generated]]),work)
    # Heap's C11 atomics use Clang's existing Windows x86 toolchain; its actual
    # allocator/ledger is linked, not a pointer-map stand-in.
    clang=Path("C:/Program Files/LLVM/bin/clang.exe");require(clang.is_file(),"installed Clang missing")
    ccmd=[str(clang),"--target=i686-pc-windows-msvc","-std=gnu11","-O2","-Wall","-Wextra","-Werror",
          "-D_CRT_SECURE_NO_WARNINGS",f"-I{RT}",
          *[x.replace("/D","-D",1) for x in defs],"-c",str(RT/"host_vita_heap.c"),
          "-o",str(work/"host_vita_heap.obj")]
    run(prefix+subprocess.list2cmdline(ccmd),work)
    outputs={}
    for mode,features in (("original",[]),("native-class",["/DISAAC_VITA_LUA_NATIVE_GETCLASS=1",
                "/DISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1"])):
        units=[RT/"host_vita_lua.c",RT/"eid_string_lifetime_oracle.c"]
        if features:units.append(RT/"host_vita_lua_getclass.c")
        out=run(prefix+" ".join([*common,"/W4","/WX","/wd4310",*defs,*features,"/c",
                                 *[R.quote(x) for x in units]]),work)
        exe=work/f"lifetime-{mode}.exe"
        objects=[x.stem+".obj" for x in lua+generated+units]+["host_vita_heap.obj"]
        run(prefix+" ".join(["link","/nologo","/LARGEADDRESSAWARE",f"/out:{R.quote(exe)}",*objects]),work)
        out=run([str(exe),str(private)],work)
        (work/f"{mode}.log").write_text(out,encoding="utf-8");print(out)
        outputs[mode]=[line for line in out.splitlines() if line.startswith("[digest]")]
    require(outputs["original"]==outputs["native-class"],"native class seam changed lifecycle/error digests")
    print("PASS: fresh original getter/copy/delete, real Lua/bridge/heap and native getClass lifecycle reference; runtime unchanged")
    return 0
if __name__=="__main__":
    try:raise SystemExit(main())
    except AssertionError as e:print(f"FAIL: {e}",file=sys.stderr);raise SystemExit(1)
