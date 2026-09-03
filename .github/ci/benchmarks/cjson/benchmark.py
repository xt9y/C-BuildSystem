#!/usr/bin/env python3
import json, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common

REPO='https://github.com/DaveGamble/cJSON.git'
REV='v1.7.19'
TARGET='cjson'
MARKER='CMakeFiles/cjson.dir'
CLEAN_RUNS=common.STANDARD_CLEAN_RUNS
NOOP_RUNS=common.STANDARD_NOOP_RUNS
CHANGE_RUNS=common.STANDARD_INCREMENTAL_RUNS
CONFIG_RUNS=common.STANDARD_CONFIG_RUNS

def cmake_args(src, build):
    return ['cmake','-S',str(src),'-B',str(build),'-G','Ninja',
            '-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            '-DCMAKE_SUPPRESS_REGENERATION=ON','-DBUILD_SHARED_LIBS=OFF',
            '-DENABLE_CJSON_TEST=OFF','-DENABLE_CJSON_UTILS=OFF',
            '-DENABLE_CUSTOM_COMPILER_FLAGS=OFF','-DENABLE_TARGET_EXPORT=OFF']

def cmake_cmd(build): return ['cmake','--build',str(build),'--target',TARGET,'--parallel','1']

def main():
    if not Path('/usr/bin/time').exists(): raise RuntimeError('GNU time required')
    common.run(['make'], cwd=common.ROOT, quiet=True)
    jobs=1
    with tempfile.TemporaryDirectory(prefix='c-cjson-bench-') as td:
        work=Path(td); src=work/'cjson'
        common.run(['git','clone','--quiet','--branch',REV,'--depth','1',REPO,str(src)])
        meta=work/'meta'; common.run(cmake_args(src,meta), quiet=True)
        cproj=work/'cproj'
        sources, flags=common.generate_build_c(meta/'compile_commands.json',cproj,marker=MARKER,source_root=src,target='bench')
        if len(sources) != 1: raise RuntimeError(f'expected cJSON library to have 1 TU, found {len(sources)}')

        cmake_config=[]
        for i in range(CONFIG_RUNS):
            b=work/f'cfg-{i}'; shutil.rmtree(b,ignore_errors=True)
            cmake_config.append(common.profiled(cmake_args(src,b)))

        nb=work/'ninja'; common.run(cmake_args(src,nb),quiet=True); ncmd=cmake_cmd(nb); common.run(ncmd,quiet=True)
        nclean=[]
        for _ in range(CLEAN_RUNS):
            common.run(['ninja','-C',str(nb),'-t','clean'],quiet=True); nclean.append(common.profiled(ncmd))
        nnoop=[common.profiled(ncmd) for _ in range(NOOP_RUNS)]
        nchange=[]
        source=sources[0]
        for _ in range(CHANGE_RUNS):
            common.git_restore(src,source); common.run(ncmd,quiet=True); time.sleep(1.05); common.touch_content(source)
            nchange.append(common.profiled(ncmd))
        common.git_restore(src,source); common.run(ncmd,quiet=True)
        before=common.object_snapshot(nb,MARKER); time.sleep(1.05); common.touch_content(source); common.run(ncmd,quiet=True); after=common.object_snapshot(nb,MARKER)
        nrebuilt=len(common.changed_objects(before,after)); common.git_restore(src,source)

        cache=work/'c-cache'; env=common.c_env(cache); ccmd=common.c_cmd(jobs)
        common.run(ccmd,cwd=cproj,env=env,quiet=True)
        cclean=[]
        for _ in range(CLEAN_RUNS):
            shutil.rmtree(cproj/'build',ignore_errors=True); cclean.append(common.profiled(ccmd,cwd=cproj,env=env))
        cnoop=[common.profiled(ccmd,cwd=cproj,env=env) for _ in range(NOOP_RUNS)]
        cchange=[]
        for _ in range(CHANGE_RUNS):
            common.git_restore(src,source); common.run(ccmd,cwd=cproj,env=env,quiet=True); time.sleep(1.05); common.touch_content(source)
            cchange.append(common.profiled(ccmd,cwd=cproj,env=env))
        common.git_restore(src,source); common.run(ccmd,cwd=cproj,env=env,quiet=True)
        before=common.object_snapshot(cproj/'build'); time.sleep(1.05); common.touch_content(source); common.run(ccmd,cwd=cproj,env=env,quiet=True); after=common.object_snapshot(cproj/'build')
        crebuilt=len(common.changed_objects(before,after)); common.git_restore(src,source); common.run(ccmd,cwd=cproj,env=env,quiet=True)

        csetup=[]
        for i in range(CONFIG_RUNS):
            fresh=work/f'fresh-cache-{i}'; shutil.rmtree(fresh,ignore_errors=True)
            csetup.append(common.profiled(ccmd,cwd=cproj,env=common.c_env(fresh)))

        if nrebuilt != 1 or crebuilt != 1: raise RuntimeError(f'unexpected rebuild count C-BuildSystem={crebuilt} Ninja={nrebuilt}')
        result={'project':'cJSON','revision':REV,'purpose':'startup/overhead','jobs':jobs,'source_count':1,
                'machine':common.machine_stats(),'semantic_flags':flags,
                'configuration':{'c_fresh_build_script_cache':common.summarize(csetup),'cmake_configure':common.summarize(cmake_config)},
                'c':{'clean':common.summarize(cclean),'noop':common.summarize(cnoop),'one_source_changed':common.summarize(cchange),'translation_units_rebuilt':crebuilt},
                'cmake_ninja':{'clean':common.summarize(nclean),'noop':common.summarize(nnoop),'one_source_changed':common.summarize(nchange),'translation_units_rebuilt':nrebuilt},
                'runs':{'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'change':CHANGE_RUNS,'configuration':CONFIG_RUNS}}
        result['standard']=common.standard_contract(
            clean_c=result['c']['clean'], clean_ninja=result['cmake_ninja']['clean'],
            noop_c=result['c']['noop'], noop_ninja=result['cmake_ninja']['noop'],
            incremental_c=result['c']['one_source_changed'], incremental_ninja=result['cmake_ninja']['one_source_changed'],
            incremental_description='one source file changed',
            rebuilt_tus={'c':crebuilt,'cmake_ninja':nrebuilt},
            runs={'clean':CLEAN_RUNS,'noop':NOOP_RUNS,'incremental':CHANGE_RUNS})
        out=common.ROOT/'benchmarks'/'cjson'; out.mkdir(parents=True,exist_ok=True)
        (out/'results.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
        def fmt(ms): return f'{ms/1000:.2f} s' if ms>=1000 else f'{ms:.1f} ms'
        md=f'''# cJSON overhead benchmark\n\nPinned cJSON {REV}, one C translation unit. Lower is better.\n\n| Test | C-BuildSystem | CMake + Ninja |\n| --- | ---: | ---: |\n| Fresh build-system setup | {fmt(result['configuration']['c_fresh_build_script_cache']['wall_ms'])} | {fmt(result['configuration']['cmake_configure']['wall_ms'])} |\n| Clean build | {fmt(result['c']['clean']['wall_ms'])} | {fmt(result['cmake_ninja']['clean']['wall_ms'])} |\n| No changes | {fmt(result['c']['noop']['wall_ms'])} | {fmt(result['cmake_ninja']['noop']['wall_ms'])} |\n| One source changed | {fmt(result['c']['one_source_changed']['wall_ms'])} | {fmt(result['cmake_ninja']['one_source_changed']['wall_ms'])} |\n\nStandard scenarios: clean={CLEAN_RUNS} runs, no-op={NOOP_RUNS}, incremental={CHANGE_RUNS}; reported wall time is the median. Object caching is disabled. Both systems rebuild exactly one TU after the source edit.\n'''
        (out/'README.md').write_text(md)
        print('CJSON_JSON='+json.dumps(result,sort_keys=True))
        print(md)
if __name__=='__main__': main()
