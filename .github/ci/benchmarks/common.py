#!/usr/bin/env python3
import json, os, shlex, shutil, statistics, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C_BIN = ROOT / 'build' / 'c'

BENCHMARK_SCHEMA_VERSION = 1
STANDARD_CLEAN_RUNS = 3
STANDARD_NOOP_RUNS = 10
STANDARD_INCREMENTAL_RUNS = 5
STANDARD_CONFIG_RUNS = 3
STATISTICS_POLICY = {
    'primary': 'median',
    'wall_time_unit': 'ms',
    'spread': ['min', 'median', 'max', 'population_stdev', 'coefficient_of_variation_percent'],
    'resource_aggregation': 'median',
    'object_cache': 'disabled',
}

TIME_FIELDS = {
    'User time (seconds)': ('user_s', float),
    'System time (seconds)': ('system_s', float),
    'Maximum resident set size (kbytes)': ('max_rss_kb', int),
    'Major (requiring I/O) page faults': ('major_faults', int),
    'Minor (reclaiming a frame) page faults': ('minor_faults', int),
    'Voluntary context switches': ('voluntary_context_switches', int),
    'Involuntary context switches': ('involuntary_context_switches', int),
    'File system inputs': ('fs_inputs', int),
    'File system outputs': ('fs_outputs', int),
}

def run(cmd, *, cwd=None, env=None, quiet=False, check=True):
    kw = dict(cwd=cwd, env=env, text=True, check=check)
    if quiet:
        kw['stdout'] = subprocess.DEVNULL
        kw['stderr'] = subprocess.DEVNULL
    return subprocess.run(cmd, **kw)

def profiled(cmd, *, cwd=None, env=None):
    fd, name = tempfile.mkstemp(prefix='c-bench-time-')
    os.close(fd)
    path = Path(name)
    try:
        start = time.perf_counter_ns()
        run(['/usr/bin/time', '-v', '-o', str(path), *cmd], cwd=cwd, env=env, quiet=True)
        wall_ms = (time.perf_counter_ns() - start) / 1e6
        values = {}
        for raw in path.read_text().splitlines():
            line = raw.strip()
            for label, (key, cast) in TIME_FIELDS.items():
                if line.startswith(label + ':'):
                    values[key] = cast(line[len(label)+1:].strip())
                    break
        missing = [k for k, _ in TIME_FIELDS.values() if k not in values]
        if missing:
            raise RuntimeError('missing GNU time fields: ' + ', '.join(missing))
        values['wall_ms'] = wall_ms
        values['cpu_percent'] = (values['user_s'] + values['system_s']) / (wall_ms / 1000.0) * 100 if wall_ms else 0.0
        return values
    finally:
        path.unlink(missing_ok=True)

def summarize(samples):
    if not samples:
        raise ValueError('benchmark summary requires at least one sample')
    out = {k: statistics.median(s[k] for s in samples) for k in samples[0]}
    walls = [s['wall_ms'] for s in samples]
    mean = statistics.mean(walls)
    stdev = statistics.pstdev(walls)
    out['stability'] = {
        'min_ms': min(walls), 'median_ms': statistics.median(walls), 'max_ms': max(walls),
        'stdev_ms': stdev, 'cv_percent': stdev / mean * 100 if mean else 0.0,
    }
    out['samples'] = samples
    return out

def as_summary(value):
    if isinstance(value, dict) and 'samples' in value and 'stability' in value:
        return value
    if isinstance(value, dict) and 'wall_ms' in value:
        return summarize([value])
    raise TypeError('benchmark scenario must be a profiled sample or summary')

def standard_contract(*, clean_c, clean_ninja, noop_c, noop_ninja,
                      incremental_c, incremental_ninja, incremental_description,
                      rebuilt_tus=None, runs=None):
    scenarios = {
        'clean': {
            'description': 'build from an empty output directory',
            'c': as_summary(clean_c),
            'cmake_ninja': as_summary(clean_ninja),
        },
        'noop': {
            'description': 'build with no source changes',
            'c': as_summary(noop_c),
            'cmake_ninja': as_summary(noop_ninja),
        },
        'incremental': {
            'description': incremental_description,
            'c': as_summary(incremental_c),
            'cmake_ninja': as_summary(incremental_ninja),
        },
    }
    if rebuilt_tus is not None:
        scenarios['incremental']['rebuilt_tus'] = rebuilt_tus
    return {
        'schema_version': BENCHMARK_SCHEMA_VERSION,
        'statistics': dict(STATISTICS_POLICY),
        'run_counts': dict(runs or {}),
        'scenarios': scenarios,
    }

def machine_stats():
    cpu = 'unknown'
    try:
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('model name'):
                cpu = line.split(':', 1)[1].strip(); break
    except OSError:
        pass
    return {
        'date': time.strftime('%Y-%m-%d', time.gmtime()),
        'platform': subprocess.check_output(['uname','-a'], text=True).strip(),
        'cpu_model': cpu, 'cpu_count': os.cpu_count(),
        'compiler': subprocess.check_output(['cc','--version'], text=True).splitlines()[0],
        'cmake': subprocess.check_output(['cmake','--version'], text=True).splitlines()[0],
        'ninja': subprocess.check_output(['ninja','--version'], text=True).strip(),
    }

def entry_args(entry):
    return list(entry['arguments']) if 'arguments' in entry else shlex.split(entry['command'])

def semantic_flags(args, source):
    out=[]; i=1
    while i < len(args):
        a=args[i]
        if a in {'-o','-MF','-MT','-MQ'}:
            i += 2; continue
        if a in {'-c','-MD','-MMD','-MP'} or a == str(source):
            i += 1; continue
        if a.startswith('-O') or a == '-g' or a.startswith('-g') or a.startswith('-W'):
            i += 1; continue
        if a in {'-I','-isystem','-include','-imacros'} and i+1 < len(args):
            out.extend([a,args[i+1]]); i += 2; continue
        if a.startswith(('-I','-D','-U','-std=','-f','-m')) or a in {'-pthread'}:
            out.append(a)
        i += 1
    return tuple(out)

def select_entries(db_path, *, marker, source_root=None):
    rows=[]
    for entry in json.loads(db_path.read_text()):
        args=entry_args(entry); joined=' '.join(args)
        src=Path(entry['file']).resolve()
        if marker not in joined:
            continue
        if src.suffix.lower() not in {'.c','.cc','.cpp','.cxx','.s'} and src.suffix != '.S':
            continue
        if source_root is not None:
            try: src.relative_to(Path(source_root).resolve())
            except ValueError: continue
        rows.append((entry,args,src))
    if not rows:
        raise RuntimeError(f'no compile_commands entries matched {marker!r}')
    return rows

def c_string(s):
    return str(s).replace('\\','\\\\').replace('"','\\"')

def generate_build_c(db_path, cproj, *, marker, source_root=None, target='bench'):
    entries=select_entries(db_path, marker=marker, source_root=source_root)
    per=[(src, semantic_flags(args, src)) for _,args,src in entries]
    baseline=per[0][1]
    mismatches=[]
    for src, flags in per[1:]:
        if flags != baseline:
            mismatches.append((src, baseline, flags))
    if mismatches:
        print(f'flag mismatch count: {len(mismatches)}')
        for src, expected, got in mismatches[:8]:
            print('mismatch:', src)
            print(' expected:', expected)
            print(' got     :', got)
        raise RuntimeError('target has per-source semantic flags; refusing unfair benchmark')
    sources=sorted({src for src,_ in per})
    lines=['#include <cbuild.h>','', 'void build(C_Build *b) {', f'    C_Target *t = c_static_library(b, "{target}");']
    for src in sources:
        lines.append(f'    c_sources(t, "{c_string(src)}");')
    flags=list(baseline); i=0
    while i < len(flags):
        a=flags[i]
        if a == '-I' and i+1 < len(flags):
            lines.append(f'    c_include(t, "{c_string(flags[i+1])}");'); i += 2; continue
        if a.startswith('-I') and len(a)>2:
            lines.append(f'    c_include(t, "{c_string(a[2:])}");'); i += 1; continue
        if a.startswith('-D') and len(a)>2:
            lines.append(f'    c_define(t, "{c_string(a[2:])}");'); i += 1; continue
        if a in {'-isystem','-include','-imacros'} and i+1 < len(flags):
            lines.append(f'    c_flag(t, "{c_string(a)}");')
            lines.append(f'    c_flag(t, "{c_string(flags[i+1])}");'); i += 2; continue
        lines.append(f'    c_flag(t, "{c_string(a)}");'); i += 1
    lines += ['}','']
    cproj.mkdir(parents=True, exist_ok=True)
    (cproj/'build.c').write_text('\n'.join(lines))
    return sources, list(baseline)

def c_env(cache):
    env=os.environ.copy(); env['C_CACHE_DIR']=str(cache); env['C_INCLUDE_DIR']=str(ROOT/'include'); env['C_OBJECT_CACHE']='0'; return env

def c_cmd(jobs, target='bench'):
    return [str(C_BIN), 'build', target, f'-j{jobs}', '--no-object-cache']

def object_snapshot(root, marker=None):
    out={}
    if not Path(root).exists(): return out
    for p in Path(root).rglob('*.o'):
        if marker and marker not in str(p): continue
        st=p.stat(); out[str(p.relative_to(root))]=(st.st_mtime_ns,st.st_size)
    return out

def changed_objects(before, after):
    return sorted(k for k,v in after.items() if k not in before or before[k][0] != v[0])

def touch_content(path, marker='/* c-buildsystem benchmark edit */'):
    with Path(path).open('a') as f: f.write('\n'+marker+'\n')

def git_restore(repo, path):
    run(['git','checkout','--quiet','--',str(path)], cwd=repo)

def measure_first_compiler(command, *, cwd, env, changed_path):
    probe=Path(cwd)/'.first-compiler-ns'
    wrapper=Path(cwd)/'.cc-bench-probe.sh'
    wrapper.write_text('#!/bin/sh\nif [ -n "$BENCH_FIRST_COMPILER_FILE" ] && mkdir "$BENCH_FIRST_COMPILER_FILE.lock" 2>/dev/null; then date +%s%N > "$BENCH_FIRST_COMPILER_FILE"; fi\nexec /usr/bin/cc "$@"\n')
    wrapper.chmod(0o755)
    e=dict(env or os.environ); e['CC']=str(wrapper); e['BENCH_FIRST_COMPILER_FILE']=str(probe)
    probe.unlink(missing_ok=True); shutil.rmtree(str(probe)+'.lock', ignore_errors=True)
    touch_content(changed_path)
    start=time.time_ns()
    run(command, cwd=cwd, env=e, quiet=True)
    if not probe.exists():
        raise RuntimeError('compiler probe did not observe a compiler invocation')
    return (int(probe.read_text().strip()) - start) / 1e6
