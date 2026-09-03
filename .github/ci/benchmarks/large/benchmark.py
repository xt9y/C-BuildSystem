#!/usr/bin/env python3
import hashlib, json, os, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common

REPO='https://github.com/wireshark/wireshark.git'
REV='wireshark-4.4.9'
TARGET='dissectors'
MARKER='CMakeFiles/dissectors.dir'
CLEAN_RUNS=common.STANDARD_CLEAN_RUNS
NOOP_RUNS=common.STANDARD_NOOP_RUNS
CHANGE_RUNS=common.STANDARD_INCREMENTAL_RUNS
CHANGE_SIZES=(1,10)

def cmake_args(src, build):
    off=['BUILD_wireshark','BUILD_tshark','BUILD_rawshark','BUILD_dumpcap','BUILD_text2pcap','BUILD_mergecap','BUILD_reordercap','BUILD_editcap','BUILD_capinfos','BUILD_captype','BUILD_randpkt','BUILD_dftest','BUILD_dcerpcidl2wrs','BUILD_androiddump','BUILD_sshdump','BUILD_ciscodump','BUILD_dpauxmon','BUILD_randpktdump','BUILD_wifidump','BUILD_sdjournal','BUILD_udpdump','BUILD_sharkd','BUILD_mmdbresolve']
    optional=['ENABLE_PCAP','ENABLE_PLUGINS','ENABLE_ZLIB','ENABLE_ZLIBNG','ENABLE_MINIZIP','ENABLE_MINIZIPNG','ENABLE_LZ4','ENABLE_BROTLI','ENABLE_SNAPPY','ENABLE_ZSTD','ENABLE_NGHTTP2','ENABLE_NGHTTP3','ENABLE_LUA','ENABLE_SMI','ENABLE_GNUTLS','ENABLE_CAP','ENABLE_NETLINK','ENABLE_KERBEROS','ENABLE_SBC','ENABLE_SPANDSP','ENABLE_BCG729','ENABLE_AMRNB','ENABLE_ILBC','ENABLE_LIBXML2','ENABLE_OPUS','ENABLE_SINSP']
    args=['cmake','-S',str(src),'-B',str(build),'-G','Ninja','-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_SUPPRESS_REGENERATION=ON','-DENABLE_STATIC=ON','-DENABLE_CCACHE=OFF','-DENABLE_WERROR=OFF','-DENABLE_COMPILER_COLOR_DIAGNOSTICS=OFF']
    args += [f'-D{x}=OFF' for x in off+optional]
    return args

def write_ninja_wrapper(path):
    path.write_text(r'''#!/usr/bin/env bash
set -euo pipefail
build="$1"
jobs="$2"
cmake --build "$build" --target dissectors --parallel "$jobs" >/dev/null
objroot="$build/epan/dissectors/CMakeFiles/dissectors.dir"
archive="$build/dissectors-benchmark.a"
need=0
if [ ! -f "$archive" ]; then
  need=1
elif find "$objroot" -name '*.o' -newer "$archive" -print -quit | grep -q .; then
  need=1
fi
if [ "$need" -eq 1 ]; then
  rm -f "$archive"
  find "$objroot" -name '*.o' -print0 | sort -z | xargs -0 -r -n 250 ar rcs "$archive"
fi
''')
    path.chmod(0o755)

def ordered_editable(sources, src):
    root=(src/'epan'/'dissectors').resolve(); out=[]
    for p in sources:
        try: rel=str(p.resolve().relative_to(root))
        except ValueError: continue
        if p.suffix.lower() != '.c': continue
        out.append((rel,p))
    out.sort(key=lambda x: hashlib.sha256(x[0].encode()).digest())
    return out

def profile_changed(repo, command, paths, objroot, *, cwd=None, env=None, marker=None):
    for p in paths: common.git_restore(repo,p)
    common.run(command,cwd=cwd,env=env,quiet=True)
    before=common.object_snapshot(objroot,marker)
    time.sleep(1.05)
    for p in paths: common.touch_content(p)
    sample=common.profiled(command,cwd=cwd,env=env)
    after=common.object_snapshot(objroot,marker)
    rebuilt=len(common.changed_objects(before,after))
    for p in paths: common.git_restore(repo,p)
    return sample,rebuilt

def main():
    common.run(['make'],cwd=common.ROOT,quiet=True)
    jobs=int(os.environ.get('BENCH_JOBS','2'))
    with tempfile.TemporaryDirectory(prefix='c-wireshark-large-') as td:
        work=Path(td); src=work/'wireshark'
        common.run(['git','clone','--quiet','--branch',REV,'--depth','1',REPO,str(src)])
        meta=work/'meta'; common.run(cmake_args(src,meta),quiet=True)
        common.run(['cmake','--build',str(meta),'--target',TARGET,'--parallel',str(jobs)],quiet=True)
        cproj=work/'cproj'; sources,flags=common.generate_build_c(meta/'compile_commands.json',cproj,marker=MARKER,target='bench')
        editable=ordered_editable(sources,src)
        if len(sources) < 500: raise RuntimeError(f'Wireshark dissector workload unexpectedly small: {len(sources)} TUs')
        if len(editable) < max(CHANGE_SIZES): raise RuntimeError(f'not enough editable dissector TUs: {len(editable)}')

        nb=work/'ninja'; common.run(cmake_args(src,nb),quiet=True)
        wrapper=work/'ninja-plus-archive.sh'; write_ninja_wrapper(wrapper); ncmd=[str(wrapper),str(nb),str(jobs)]
        common.run(ncmd,quiet=True)
        nclean=[]
        for _ in range(CLEAN_RUNS):
            common.run(['ninja','-C',str(nb),'-t','clean'],quiet=True); (nb/'dissectors-benchmark.a').unlink(missing_ok=True); nclean.append(common.profiled(ncmd))
        nnoop=[common.profiled(ncmd) for _ in range(NOOP_RUNS)]

        cache=work/'c-cache'; ce=common.c_env(cache); ccmd=common.c_cmd(jobs); common.run(ccmd,cwd=cproj,env=ce,quiet=True)
        cclean=[]
        for _ in range(CLEAN_RUNS):
            shutil.rmtree(cproj/'build',ignore_errors=True); cclean.append(common.profiled(ccmd,cwd=cproj,env=ce))
        cnoop=[common.profiled(ccmd,cwd=cproj,env=ce) for _ in range(NOOP_RUNS)]

        points=[]
        for count in CHANGE_SIZES:
            paths=[p for _,p in editable[:count]]
            nsamples=[]; csamples=[]; ncounts=[]; ccounts=[]
            for _ in range(CHANGE_RUNS):
                ns,nr=profile_changed(src,ncmd,paths,nb,marker=MARKER)
                cs,cr=profile_changed(src,ccmd,paths,cproj/'build',cwd=cproj,env=ce)
                nsamples.append(ns); csamples.append(cs); ncounts.append(nr); ccounts.append(cr)
            if set(ncounts) != {count} or set(ccounts) != {count}:
                raise RuntimeError(f'{count}-source point rebuilt unexpected TUs: C-BuildSystem={ccounts} Ninja={ncounts}')
            points.append({'changed_sources':count,'source_paths':[r for r,_ in editable[:count]],
                           'c':common.summarize(csamples),'cmake_ninja':common.summarize(nsamples),
                           'rebuilt_tus':count,'runs':CHANGE_RUNS})

        result={'project':'Wireshark dissectors','revision':REV,'purpose':'large translation-unit stress','target':TARGET,'source_count':len(sources),'editable_source_count':len(editable),'jobs':jobs,'machine':common.machine_stats(),'semantic_flags':flags,
                'c':{'clean':common.summarize(cclean),'noop':common.summarize(cnoop)},'cmake_ninja':{'clean':common.summarize(nclean),'noop':common.summarize(nnoop)},'points':points,
                'runs':{'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'incremental':CHANGE_RUNS},
                'method':'same CMake-derived source set and semantic flags; Ninja object target is followed by a timestamp-aware static archive step to match C-BuildSystem static-library output'}
        result['standard']=common.standard_contract(
            clean_c=result['c']['clean'], clean_ninja=result['cmake_ninja']['clean'],
            noop_c=result['c']['noop'], noop_ninja=result['cmake_ninja']['noop'],
            incremental_c=points[0]['c'], incremental_ninja=points[0]['cmake_ninja'],
            incremental_description='one source file changed',
            rebuilt_tus={'c':1,'cmake_ninja':1},
            runs={'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'incremental':CHANGE_RUNS})
        out=common.ROOT/'benchmarks'/'large'; out.mkdir(parents=True,exist_ok=True); (out/'results.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
        def fmt(x): return f'{x/1000:.2f} s' if x>=1000 else f'{x:.1f} ms'
        rows='\n'.join(f"| {p['changed_sources']} source{'s' if p['changed_sources']!=1 else ''} changed | {fmt(p['c']['wall_ms'])} | {fmt(p['cmake_ninja']['wall_ms'])} |" for p in points)
        md=f'''# Wireshark large-project stress benchmark\n\nPinned Wireshark `{REV}` dissector workload: **{len(sources)} translation units**, {jobs} build jobs. Lower is better.\n\n| Test | C-BuildSystem | CMake + Ninja |\n| --- | ---: | ---: |\n| Clean compile + archive | {fmt(result['c']['clean']['wall_ms'])} | {fmt(result['cmake_ninja']['clean']['wall_ms'])} |\n| No changes | {fmt(result['c']['noop']['wall_ms'])} | {fmt(result['cmake_ninja']['noop']['wall_ms'])} |\n{rows}\n\nStandard scenarios: clean={CLEAN_RUNS} runs, no-op={NOOP_RUNS}, incremental={CHANGE_RUNS}; reported wall time is the median. The 10-source point is an additional stress scenario. Object caching is disabled.\n'''
        (out/'README.md').write_text(md); print('LARGE_JSON='+json.dumps(result,sort_keys=True)); print(md)
if __name__=='__main__': main()
