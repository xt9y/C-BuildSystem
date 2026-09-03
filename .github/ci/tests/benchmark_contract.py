#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'benchmarks'
sys.path.insert(0, str(BENCH))
import common

assert common.BENCHMARK_SCHEMA_VERSION == 1
assert common.STANDARD_CLEAN_RUNS == 3
assert common.STANDARD_NOOP_RUNS == 10
assert common.STANDARD_INCREMENTAL_RUNS == 5
assert common.STANDARD_CONFIG_RUNS == 3
assert common.STATISTICS_POLICY['primary'] == 'median'
assert common.STATISTICS_POLICY['wall_time_unit'] == 'ms'
assert common.STATISTICS_POLICY['object_cache'] == 'disabled'

sample = {'wall_ms': 10.0, 'user_s': 0.01}
summary = common.summarize([sample, dict(sample, wall_ms=12.0), dict(sample, wall_ms=11.0)])
assert summary['wall_ms'] == 11.0
assert summary['stability']['median_ms'] == 11.0
assert len(summary['samples']) == 3

contract = common.standard_contract(
    clean_c=summary, clean_ninja=summary,
    noop_c=summary, noop_ninja=summary,
    incremental_c=summary, incremental_ninja=summary,
    incremental_description='test edit', rebuilt_tus={'c': 1, 'cmake_ninja': 1},
    runs={'clean': 3, 'noop': 10, 'incremental': 5},
)
assert contract['schema_version'] == 1
assert set(contract['scenarios']) == {'clean', 'noop', 'incremental'}
assert contract['run_counts'] == {'clean': 3, 'noop': 10, 'incremental': 5}
assert contract['scenarios']['incremental']['rebuilt_tus'] == {'c': 1, 'cmake_ninja': 1}

for relative in [
    'cjson/benchmark.py',
    'fanout/benchmark.py',
    'large/benchmark.py',
    'sdl3/benchmark_stats.py',
]:
    text = (BENCH / relative).read_text()
    assert 'common.STANDARD_CLEAN_RUNS' in text, relative
    assert 'common.STANDARD_NOOP_RUNS' in text, relative
    assert 'common.STANDARD_INCREMENTAL_RUNS' in text, relative
    assert 'common.standard_contract' in text, relative

print('benchmark-contract: ok')
