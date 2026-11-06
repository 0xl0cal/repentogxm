#!/usr/bin/env python3
"""Exact native-domain slab rejection: deterministic traces, threads, reused
J835/Lua lifetimes, old regression closure, OFF source/object identity. No Vita.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import subprocess
import sys
import test_vita_lua_runtime as R

ROOT=Path(__file__).resolve().parent
RT=ROOT/"runtime"
BASE="91c374cd98288ea4f709d77ef26cbb89bde6449e"
FLAG="ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH"
def need(ok:bool,msg:str)->None:
    if not ok:raise AssertionError(msg)
def run(cmd:str|list[str],cwd:Path)->str:
    p=subprocess.run(cmd,cwd=cwd,shell=isinstance(cmd,str),text=True,
                     stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    need(p.returncode==0,f"command failed {p.returncode}: {cmd}\n{p.stdout}")
    return p.stdout
def quoted(argv:list[str])->str:return subprocess.list2cmdline(argv)
def norm_obj(data:bytes)->bytes:
    # COFF timestamp is metadata, not code. Everything else must match.
    return data[:4]+b"\0\0\0\0"+data[8:]
def main()->int:
    p=argparse.ArgumentParser()
    p.add_argument("--pe",type=Path,required=True)
    p.add_argument("--lua-source",type=Path,required=True)
    p.add_argument("--work",type=Path,required=True)
    args=p.parse_args();w=args.work.resolve();w.mkdir(parents=True,exist_ok=True)
    fake=w/"fake-include"
    text=(ROOT/"vita/test_heap_overflow_router.sh").read_text()
    headers=re.findall(r'cat > "\$fake_include/([^\"]+)" <<\x27EOF\x27\n(.*?)\nEOF',text,re.S)
    need(len(headers)==3,"original fake native header closure changed")
    for name,body in headers:
        dest=fake/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(body+"\n")
    # Test-only Windows equivalent of the two pthread calls the EXISTING
    # terminal observer oracle uses; return values are retained, not faked.
    (fake/"pthread.h").write_text('''#include <windows.h>
#include <stdlib.h>
typedef struct pt_ctx {void *(*fn)(void *);void *arg,*result;} pt_ctx;
typedef struct pthread_t {HANDLE h;pt_ctx *ctx;} pthread_t;
static DWORD WINAPI pt_run(void *p){pt_ctx *c=p;c->result=c->fn(c->arg);return 0;}
static int pthread_create(pthread_t *t,void *attr,void *(*fn)(void *),void *arg){
 (void)attr;t->ctx=malloc(sizeof *t->ctx);if(!t->ctx)return 1;
 t->ctx->fn=fn;t->ctx->arg=arg;t->ctx->result=NULL;t->h=CreateThread(NULL,0,pt_run,t->ctx,0,NULL);
 if(!t->h){free(t->ctx);return 1;}return 0;}
static int pthread_join(pthread_t t,void **result){
 if(WaitForSingleObject(t.h,INFINITE)!=WAIT_OBJECT_0)return 1;
 if(result)*result=t.ctx->result;CloseHandle(t.h);free(t.ctx);return 0;}
''')
    clang=Path("C:/Program Files/LLVM/bin/clang.exe");need(clang.is_file(),"Clang must be installed")
    vc=R.find_vcvars();prefix64=f"call {R.quote(vc)} x64 >nul && "
    defs=["-DGUEST_IMAGE_BASE=0x98000000u","-DISAAC_VITA_HEAP_TESTING=1",
          "-DISAAC_VITA_HEAP_RANGE_LEASE=1","-DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1",
          "-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1","-DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1",
          "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1","-DISAAC_VITA_ROOM_ENTRY_SLAB=1",
          "-DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1","-DISAAC_VITA_ROOM_ENTRY_HYBRID=1",
          "-DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1","-DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1",
          "-DISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x017fd000U",
          "-DISAAC_VITA_HEAP_ORACLE_NO_LINKER_WRAP=1"]
    cc=[str(clang),"-std=gnu11","-O2","-Wall","-Wextra","-Werror","-D_CRT_SECURE_NO_WARNINGS",
        f"-I{fake}",f"-I{RT}"]
    modules=["host_vita_heap.c","host_vita_heap_overflow_mspace.c","host_vita_heap_ledger_memblock.c",
             "host_vita_room_entry_slab.c","host_vita_room_entry_external.c"]
    interpose=["-Dmalloc=fixture_malloc","-Dcalloc=fixture_calloc","-Drealloc=fixture_realloc","-Dfree=fixture_free"]
    # Same source PATH for all identity compiles, so debug/file-name metadata
    # cannot manufacture a difference. Inputs are freshly created each run.
    identity=w/"identity";identity.mkdir(exist_ok=True)
    source=identity/"host_vita_heap.c"
    current=(RT/"host_vita_heap.c").read_text()
    legs=[("off",current,[]),("zero",current,[f"-D{FLAG}=0"])]
    have_base=subprocess.run(["git","cat-file","-e",BASE+"^{commit}"],cwd=ROOT.parent,
                             capture_output=True).returncode==0
    if have_base:
        legs.insert(0,("base",run(["git","show",BASE+":recomp/runtime/host_vita_heap.c"],ROOT.parent),[]))
    else:
        print(f"OFF identity vs {BASE[:7]}: SKIP (base commit is not in this repository's history);"
              " absent vs explicit-zero still compared")
    objects={};tokens={}
    for tag,data,feature in legs:
        source.write_text(data);obj=identity/(tag+".obj")
        run(prefix64+quoted([*cc,*defs,*feature,"-c",str(source),"-o",str(obj)]),w)
        tokens[tag]=run(prefix64+quoted([*cc,*defs,*feature,"-E","-P",str(source)]),w)
        objects[tag]=norm_obj(obj.read_bytes())
    need(len(set(tokens.values()))==1,"OFF/zero preprocess identity failed")
    need(len(set(objects.values()))==1,"OFF/zero whole COFF object identity failed")
    print("OFF/explicit-zero preprocessed + whole host COFF object identity"
          +(" vs91c374c" if have_base else "")+": PASS (timestamp normalized only)")
    trace={}
    for tag,feature in (("off",[]),("on",[f"-D{FLAG}=1"])):
        units=[]
        for name in modules:
            obj=w/(tag+"-"+Path(name).stem+".obj")
            run(prefix64+quoted([*cc,*defs,*(feature+interpose if name=="host_vita_heap.c" else []),
                                "-c",str(RT/name),"-o",str(obj)]),w);units.append(obj)
        for name in ("heap_fastpath_fixture.c","heap_fastpath_trace_oracle.c"):
            obj=w/(tag+"-"+Path(name).stem+".obj")
            run(prefix64+quoted([*cc,*defs,"-c",str(RT/name),"-o",str(obj)]),w);units.append(obj)
        exe=w/("trace-"+tag+".exe")
        run(prefix64+quoted([str(clang),*[str(x) for x in units],"-o",str(exe),
                            "-Xlinker","/BASE:0x20000000","-Xlinker","/DYNAMICBASE:NO"]),w)
        out=run([str(exe)],w);(w/("trace-"+tag+".log")).write_text(out);print(out)
        trace[tag]=next(line for line in out.splitlines() if line.startswith("[trace]"))
    need(trace["off"]==trace["on"],"same deterministic allocation/free trace changed pointers/accounting/observer side effects")
    print("PASS deterministic old/new returned-pointer, domain/failure/accounting/observer trace equality; contention executed separately")
    # Existing native/kernel oracle, including lease refusal, migration rollback,
    # ledger ceiling, terminal states, and slab/external teardown. Only adapt its
    # fixture's unsupported MSVC static 64KiB alignment (same prior host method).
    oracle=(RT/"host_vita_heap_overflow_router_oracle.c").read_text()
    declaration=("static _Alignas(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES)\n"
                 "unsigned char s_oracle_room_external_blocks[\n"
                 "    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS][0x00110000U];")
    need(oracle.count(declaration)==1,"existing external fixture storage drift")
    oracle=oracle.replace(declaration,
        "#include <windows.h>\n"
        "typedef unsigned char external_fixture[ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS][0x00110000U];\n"
        "static external_fixture *external_pointer;\n"
        "#define s_oracle_room_external_blocks (*external_pointer)")
    oracle=oracle.replace("int main(int argc, char **argv)","static int existing_main(int argc, char **argv)")
    oracle+=("\nint main(int argc,char **argv){\n"
             " external_pointer=VirtualAlloc((void *)(uintptr_t)0x70000000U,sizeof(external_fixture),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);\n"
             " if((uintptr_t)external_pointer!=0x70000000U)return 90;\n"
             " return existing_main(argc,argv);\n}\n")
    adapted=w/"windows-existing-router-oracle.c";adapted.write_text(oracle)
    for tag,feature in (("off",[]),("on",[f"-D{FLAG}=1"])):
        units=[]
        for name in modules+["host_vita_heap_overflow_router_oracle.c"]:
            obj=w/("regression-"+tag+"-"+Path(name).stem+".obj")
            rename=[x.replace("fixture_","oracle_native_") for x in interpose]
            run(prefix64+quoted([*cc,*defs,*feature,*(rename if name=="host_vita_heap.c" else []),
                "-c",str(adapted if name=="host_vita_heap_overflow_router_oracle.c" else RT/name),"-o",str(obj)]),w)
            units.append(obj)
        exe=w/("regression-"+tag+".exe")
        run(prefix64+quoted([str(clang),*[str(x) for x in units],"-o",str(exe),
            "-Xlinker","/BASE:0x20000000","-Xlinker","/DYNAMICBASE:NO","-Xlinker","/ALIGN:65536"]),w)
        for mode in ([],["--terminal-observer"],["--room-commit-terminal"],
                     ["--room-invariant-terminal"]):
            # --room-cold-invariant-terminal needs GNU ld --wrap (fails OFF too here).
            out=run([str(exe),*mode],w)
            (w/("regression-"+tag+(mode[0] if mode else "-full")+".log")).write_text(out);print(out)
    # Regenerate the lifetime corpus through its existing pinned runner FIRST.
    # That run independently exercises all23 original bodies and the two class modes.
    lw=w/"lifetime";out=run([sys.executable,str(ROOT/"test_eid_lua_lifetime.py"),
        "--pe",str(args.pe),"--lua-source",str(args.lua_source),"--work",str(lw)],w)
    (w/"lifetime-original.log").write_text(out);print(out)
    lua_source=R.discover_source(args.lua_source)
    libs=[x for x in R.validate_source(lua_source,R.records()) if x.name not in ("lua.c","luac.c")]
    prefix32=f"call {R.quote(vc)} x86 >nul && "
    generated=sorted(lw.glob("sub_*.obj"))+sorted(lw.glob("__real_*.obj"))
    need(len(generated)==23,"fresh lifetime generated object closure changed")
    lifetime={}
    for tag,feature in (("off",[]),("on",[f"-D{FLAG}=1"])):
        units=[]
        for name in modules+["heap_fastpath_fixture.c"]:
            obj=lw/(tag+"-"+Path(name).stem+".obj")
            command=[*cc,"--target=i686-pc-windows-msvc",*defs,
                     *(feature+interpose if name=="host_vita_heap.c" else []),"-c",str(RT/name),"-o",str(obj)]
            run(prefix32+quoted(command),lw);units.append(obj)
        common=["cl","/nologo","/std:c11","/O2","/W4","/WX","/wd4310","/D_CRT_SECURE_NO_WARNINGS",
                f"/I{R.quote(lua_source/'src')}",f"/I{R.quote(RT)}",f"/I{R.quote(fake)}",
                *[x.replace("-D","/D",1) for x in defs],"/DGUEST_GPR_LOCAL=1","/DGUEST_FLAGS_LOCAL=1",
                "/DISAAC_HEAP_FASTPATH_FIXTURE=1"]
        run(prefix32+" ".join([*common,"/c",R.quote(RT/"host_vita_lua.c"),
                                 R.quote(RT/"eid_string_lifetime_oracle.c")]),lw)
        exe=lw/("deterministic-lifetime-"+tag+".exe")
        objs=[x.stem+".obj" for x in libs]+[str(x) for x in generated+units]+["host_vita_lua.obj","eid_string_lifetime_oracle.obj"]
        run(prefix32+" ".join(["link","/nologo","/LARGEADDRESSAWARE",f"/out:{R.quote(exe)}",*[R.quote(x) for x in objs]]),lw)
        out=run([str(exe),str(lw/"fresh-image.bin")],lw);(w/("lifetime-"+tag+".log")).write_text(out);print(out)
        lifetime[tag]=[line for line in out.splitlines() if line.startswith("[digest]")]
    need(lifetime["off"]==lifetime["on"],"14009-allocation lifetime counts/errors/teardown changed")
    print("PASS reused14009 successful allocation/free lifetimes OFF/ON: counts, error recovery, LuaRef order, cold teardown")
    return 0
if __name__=="__main__":
    try:raise SystemExit(main())
    except AssertionError as e:print(f"FAIL: {e}",file=sys.stderr);raise SystemExit(1)
