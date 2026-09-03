#!/usr/bin/env python3
import json, os, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common

REPO='https://github.com/curl/curl.git'
REV='curl-8_21_0'
TARGET='libcurl_static'
MARKER='CMakeFiles/libcurl_static.dir'
HEADER_REL='lib/curl_setup.h'
FANOUT_RUNS=common.STANDARD_INCREMENTAL_RUNS
NOOP_RUNS=common.STANDARD_NOOP_RUNS
CLEAN_RUNS=common.STANDARD_CLEAN_RUNS

def cmake_args(src, build, launcher=None):
    args=['cmake','-S',str(src),'-B',str(build),'-G','Ninja',
          '-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
          '-DCMAKE_SUPPRESS_REGENERATION=ON','-DBUILD_SHARED_LIBS=OFF','-DBUILD_STATIC_LIBS=ON',
          '-DBUILD_CURL_EXE=OFF','-DCURL_BUILD_TESTING=OFF','-DBUILD_TESTING=OFF',
          '-DPICKY_COMPILER=OFF','-DCURL_WERROR=OFF','-DCURL_USE_LIBPSL=OFF',
          '-DCURL_USE_OPENSSL=OFF','-DCURL_USE_GNUTLS=OFF','-DCURL_USE_MBEDTLS=OFF',
          '-DCURL_USE_WOLFSSL=OFF','-DCURL_USE_RUSTLS=OFF','-DCURL_USE_LIBSSH2=OFF',
          '-DENABLE_ARES=OFF','-DCURL_DISABLE_INSTALL=ON']
    if launcher: args.append(f'-DCMAKE_C_COMPILER_LAUNCHER={launcher}')
    return args

def ncmd(build,jobs): return ['cmake','--build',str(build),'--target',TARGET,'--parallel',str(jobs)]

def write_launcher(path, delegate_cc=False):
    tail='exec /usr/bin/cc "$@"' if delegate_cc else 'exec "$@"'
    path.write_text('#!/bin/sh\nif [ -n "$BENCH_FIRST_COMPILER_FILE" ] && mkdir "$BENCH_FIRST_COMPILER_FILE.lock" 2>/dev/null; then date +%s%N > "$BENCH_FIRST_COMPILER_FILE"; fi\n'+tail+'\n')
    path.chmod(0o755)

def first_compile_ms(command, *, cwd, env, probe):
    probe.unlink(missing_ok=True); shutil.rmtree(str(probe)+'.lock',ignore_errors=True)
    e=dict(env); e['BENCH_FIRST_COMPILER_FILE']=str(probe)
    start=time.time_ns(); common.run(command,cwd=cwd,env=e,quiet=True)
    if not probe.exists(): raise RuntimeError('first-compiler probe saw no compiler process')
    return (int(probe.read_text().strip())-start)/1e6

def main():
    common.run(['make'],cwd=common.ROOT,quiet=True)
    jobs=int(os.environ.get('BENCH_JOBS','2'))
    with tempfile.TemporaryDirectory(prefix='c-curl-fanout-') as td:
        work=Path(td); src=work/'curl'; common.run(['git','clone','--quiet','--branch',REV,'--depth','1',REPO,str(src)])
        header=src/HEADER_REL
        meta=work/'meta'; common.run(cmake_args(src,meta),quiet=True)
        cproj=work/'cproj'; sources,flags=common.generate_build_c(meta/'compile_commands.json',cproj,marker=MARKER,source_root=src/'lib',target='bench')
        source_count=len(sources)
        if source_count < 50: raise RuntimeError(f'libcurl target unexpectedly small: {source_count} TUs')

        nb=work/'ninja'; common.run(cmake_args(src,nb),quiet=True); nc=ncmd(nb,jobs); common.run(nc,quiet=True)
        cn=[]
        for _ in range(CLEAN_RUNS):
            common.run(['ninja','-C',str(nb),'-t','clean'],quiet=True); cn.append(common.profiled(nc))
        nn=[common.profiled(nc) for _ in range(NOOP_RUNS)]
        nf=[]; ncounts=[]
        for _ in range(FANOUT_RUNS):
            common.git_restore(src,header); common.run(nc,quiet=True); before=common.object_snapshot(nb,MARKER); time.sleep(1.05); common.touch_content(header)
            nf.append(common.profiled(nc)); after=common.object_snapshot(nb,MARKER); ncounts.append(len(common.changed_objects(before,after)))
        common.git_restore(src,header); common.run(nc,quiet=True)

        cache=work/'c-cache'; ce=common.c_env(cache); cc=common.c_cmd(jobs); common.run(cc,cwd=cproj,env=ce,quiet=True)
        cclean=[]
        for _ in range(CLEAN_RUNS):
            shutil.rmtree(cproj/'build',ignore_errors=True); cclean.append(common.profiled(cc,cwd=cproj,env=ce))
        cnoop=[common.profiled(cc,cwd=cproj,env=ce) for _ in range(NOOP_RUNS)]
        cf=[]; ccounts=[]
        for _ in range(FANOUT_RUNS):
            common.git_restore(src,header); common.run(cc,cwd=cproj,env=ce,quiet=True); before=common.object_snapshot(cproj/'build'); time.sleep(1.05); common.touch_content(header)
            cf.append(common.profiled(cc,cwd=cproj,env=ce)); after=common.object_snapshot(cproj/'build'); ccounts.append(len(common.changed_objects(before,after)))
        common.git_restore(src,header); common.run(cc,cwd=cproj,env=ce,quiet=True)

        if len(set(ncounts)) != 1 or len(set(ccounts)) != 1: raise RuntimeError(f'unstable fanout counts C-BuildSystem={ccounts} Ninja={ncounts}')
        if ccounts[0] != ncounts[0]: raise RuntimeError(f'different invalidation sets by count: C-BuildSystem={ccounts[0]} Ninja={ncounts[0]}')
        if ccounts[0] < source_count//2: raise RuntimeError(f'{HEADER_REL} only invalidated {ccounts[0]}/{source_count} TUs')

        launcher=work/'compiler-launcher.sh'; write_launcher(launcher,False)
        nlat=work/'ninja-latency'; common.run(cmake_args(src,nlat,launcher),quiet=True); nlc=ncmd(nlat,jobs); common.run(nlc,quiet=True)
        common.git_restore(src,header); common.run(nlc,quiet=True); time.sleep(1.05); common.touch_content(header)
        nlat_ms=first_compile_ms(nlc,cwd=work,env=os.environ.copy(),probe=work/'ninja-first-ns')
        common.git_restore(src,header); common.run(nlc,quiet=True)

        cwrap=work/'cc-wrapper.sh'; write_launcher(cwrap,True); cle=common.c_env(work/'c-latency-cache'); cle['CC']=str(cwrap)
        common.run(cc,cwd=cproj,env=cle,quiet=True)
        common.git_restore(src,header); common.run(cc,cwd=cproj,env=cle,quiet=True); time.sleep(1.05); common.touch_content(header)
        clat_ms=first_compile_ms(cc,cwd=cproj,env=cle,probe=work/'c-first-ns')
        common.git_restore(src,header)

        result={'project':'libcurl','revision':REV,'purpose':'header fan-out / dependency invalidation','header':HEADER_REL,
                'source_count':source_count,'invalidated_tus':ccounts[0],'jobs':jobs,'machine':common.machine_stats(),'semantic_flags':flags,
                'c':{'clean':common.summarize(cclean),'noop':common.summarize(cnoop),'header_change':common.summarize(cf),'invocation_to_first_compiler_ms':clat_ms,'rebuilt_counts':ccounts},
                'cmake_ninja':{'clean':common.summarize(cn),'noop':common.summarize(nn),'header_change':common.summarize(nf),'invocation_to_first_compiler_ms':nlat_ms,'rebuilt_counts':ncounts},
                'runs':{'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'fanout':FANOUT_RUNS}}
        result['standard']=common.standard_contract(
            clean_c=result['c']['clean'], clean_ninja=result['cmake_ninja']['clean'],
            noop_c=result['c']['noop'], noop_ninja=result['cmake_ninja']['noop'],
            incremental_c=result['c']['header_change'], incremental_ninja=result['cmake_ninja']['header_change'],
            incremental_description=f'header fan-out edit to {HEADER_REL}',
            rebuilt_tus={'c':ccounts[0],'cmake_ninja':ncounts[0]},
            runs={'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'incremental':FANOUT_RUNS})
        out=common.ROOT/'benchmarks'/'fanout'; out.mkdir(parents=True,exist_ok=True); (out/'results.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
        def fmt(x): return f'{x/1000:.2f} s' if x>=1000 else f'{x:.1f} ms'
        md=f'''# libcurl header fan-out benchmark\n\nPinned libcurl {REV}. A harmless content change is made to `{HEADER_REL}`. Both systems must invalidate the same number of translation units.\n\n- Target translation units: **{source_count}**\n- Translation units invalidated: **{ccounts[0]}**\n\n| Test | C-BuildSystem | CMake + Ninja |\n| --- | ---: | ---: |\n| No changes | {fmt(result['c']['noop']['wall_ms'])} | {fmt(result['cmake_ninja']['noop']['wall_ms'])} |\n| Header fan-out rebuild | {fmt(result['c']['header_change']['wall_ms'])} | {fmt(result['cmake_ninja']['header_change']['wall_ms'])} |\n| Invocation to first compiler | {fmt(clat_ms)} | {fmt(nlat_ms)} |\n| Clean build | {fmt(result['c']['clean']['wall_ms'])} | {fmt(result['cmake_ninja']['clean']['wall_ms'])} |\n\nStandard scenarios: clean={CLEAN_RUNS} runs, no-op={NOOP_RUNS}, incremental={FANOUT_RUNS}; reported wall time is the median. Object caching is disabled.\n'''
        (out/'README.md').write_text(md); print('FANOUT_JSON='+json.dumps(result,sort_keys=True)); print(md)
if __name__=='__main__': main()
