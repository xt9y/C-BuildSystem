#!/usr/bin/env python3

import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common
import benchmark_impl as impl

BASE = "b07d48821698af08545cb38e293ead99753bfc35"
CHANGE = "eba3c7ae0ad85c13051179d196e5187ccb96cf6a"
CHANGE_FILE = "src/joystick/SDL_gamepad.c"
CLEAN_RUNS = common.STANDARD_CLEAN_RUNS
NOOP_RUNS = common.STANDARD_NOOP_RUNS
CHANGE_RUNS = common.STANDARD_INCREMENTAL_RUNS
ARCHIVE_RUNS = 5
CONFIG_RUNS = common.STANDARD_CONFIG_RUNS

ROOT = Path(__file__).resolve().parents[2]
C_BIN = ROOT / "build" / "c"

impl.BASE = BASE
impl.CHANGE = CHANGE
impl.CHANGE_FILE = CHANGE_FILE
_base_cmake_args = impl.cmake_args


def cmake_args(source, build):
    return _base_cmake_args(source, build) + [
        "-DSDL_UNIX_CONSOLE_BUILD=ON",
        "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
        "-DSDL_REVISION=benchmark",
    ]


impl.cmake_args = cmake_args


def profiled(cmd, *, cwd=None, env=None):
    return common.profiled(cmd, cwd=cwd, env=env)


def summarize(samples):
    return common.summarize(samples)


def checkout_with_gap(repo, revision):
    time.sleep(1.05)
    impl.checkout(repo, revision)


def object_snapshot(root, marker=None):
    result = {}
    for path in root.rglob("*.o"):
        if marker and marker not in str(path):
            continue
        stat = path.stat()
        result[str(path.relative_to(root))] = (stat.st_mtime_ns, stat.st_size)
    return result


def changed_objects(before, after):
    return sorted(path for path, state in after.items() if path not in before or before[path][0] != state[0])


def find_archive(root):
    matches = list(root.rglob("libSDL3.a")) + list(root.rglob("SDL3.a"))
    if not matches:
        raise RuntimeError(f"SDL3 static archive not found under {root}")
    return sorted(matches, key=lambda p: (len(p.parts), str(p)))[0]


def output_stats(root, marker=None):
    objects = [p for p in root.rglob("*.o") if not marker or marker in str(p)]
    archive = find_archive(root)
    return {
        "archive_path": str(archive.relative_to(root)),
        "archive_bytes": archive.stat().st_size,
        "object_bytes": sum(p.stat().st_size for p in objects),
        "object_count": len(objects),
    }


def change_stats(repo):
    commits = int(subprocess.check_output(["git", "rev-list", "--count", f"{BASE}..{CHANGE}"], cwd=repo, text=True).strip())
    raw = subprocess.check_output(["git", "diff", "--numstat", BASE, CHANGE, "--"], cwd=repo, text=True)
    files = []
    additions = deletions = 0
    for line in raw.splitlines():
        add, delete, path = line.split("\t", 2)
        add_n = 0 if add == "-" else int(add)
        del_n = 0 if delete == "-" else int(delete)
        additions += add_n
        deletions += del_n
        files.append({"path": path, "additions": add_n, "deletions": del_n})
    return {
        "commits": commits,
        "files_changed": len(files),
        "additions": additions,
        "deletions": deletions,
        "files": files,
    }


def machine_stats():
    return common.machine_stats()


def c_env(cache):
    env = os.environ.copy()
    env["C_CACHE_DIR"] = str(cache)
    env["C_INCLUDE_DIR"] = str(ROOT / "include")
    env["C_OBJECT_CACHE"] = "0"
    return env


def c_cmd(jobs):
    return [str(C_BIN), "build", "SDL3", f"-j{jobs}", "--no-object-cache"]


def profile_cmake_config(repo, work):
    samples = []
    for index in range(CONFIG_RUNS):
        build = work / f"cmake-config-{index}"
        shutil.rmtree(build, ignore_errors=True)
        samples.append(profiled(cmake_args(repo, build)))
    return summarize(samples)


def profile_c_first_run(cproj, work, jobs):
    samples = []
    for index in range(CONFIG_RUNS):
        cache = work / f"c-first-run-{index}"
        shutil.rmtree(cache, ignore_errors=True)
        samples.append(profiled(c_cmd(jobs), cwd=cproj, env=c_env(cache)))
    return summarize(samples)


def ninja_stats(repo, work, jobs):
    build = work / "ninja-stats"
    impl.run(cmake_args(repo, build), quiet=True)
    command = impl.cmake_build_cmd(build, jobs)
    impl.run(command, quiet=True)

    clean = []
    for _ in range(CLEAN_RUNS):
        impl.run(["ninja", "-C", str(build), "-t", "clean"], quiet=True)
        clean.append(profiled(command))

    noop = [profiled(command) for _ in range(NOOP_RUNS)]

    update = []
    for _ in range(CHANGE_RUNS):
        impl.checkout(repo, BASE)
        impl.run(command, quiet=True)
        checkout_with_gap(repo, CHANGE)
        update.append(profiled(command))

    impl.checkout(repo, BASE)
    impl.run(command, quiet=True)
    archive = []
    for _ in range(ARCHIVE_RUNS):
        find_archive(build).unlink()
        archive.append(profiled(command))

    impl.checkout(repo, BASE)
    impl.run(command, quiet=True)
    before = object_snapshot(build, marker="SDL3-static.dir")
    checkout_with_gap(repo, CHANGE)
    impl.run(command, quiet=True)
    after = object_snapshot(build, marker="SDL3-static.dir")
    rebuilt = changed_objects(before, after)

    return {
        "clean": summarize(clean),
        "noop": summarize(noop),
        "real_update": summarize(update),
        "archive_only": summarize(archive),
        "translation_units_rebuilt": len(rebuilt),
        "rebuilt_objects": rebuilt,
        "output": output_stats(build, marker="SDL3-static.dir"),
    }


def c_stats(repo, cproj, work, jobs):
    cache = work / "c-stats-cache"
    env = c_env(cache)
    command = c_cmd(jobs)
    impl.run(command, cwd=cproj, env=env, quiet=True)

    clean = []
    for _ in range(CLEAN_RUNS):
        shutil.rmtree(cproj / "build", ignore_errors=True)
        clean.append(profiled(command, cwd=cproj, env=env))

    noop = [profiled(command, cwd=cproj, env=env) for _ in range(NOOP_RUNS)]

    update = []
    for _ in range(CHANGE_RUNS):
        impl.checkout(repo, BASE)
        impl.run(command, cwd=cproj, env=env, quiet=True)
        checkout_with_gap(repo, CHANGE)
        update.append(profiled(command, cwd=cproj, env=env))

    impl.checkout(repo, BASE)
    impl.run(command, cwd=cproj, env=env, quiet=True)
    archive = []
    for _ in range(ARCHIVE_RUNS):
        find_archive(cproj / "build").unlink()
        archive.append(profiled(command, cwd=cproj, env=env))

    impl.checkout(repo, BASE)
    impl.run(command, cwd=cproj, env=env, quiet=True)
    before = object_snapshot(cproj / "build")
    checkout_with_gap(repo, CHANGE)
    impl.run(command, cwd=cproj, env=env, quiet=True)
    after = object_snapshot(cproj / "build")
    rebuilt = changed_objects(before, after)

    return {
        "clean": summarize(clean),
        "noop": summarize(noop),
        "real_update": summarize(update),
        "archive_only": summarize(archive),
        "translation_units_rebuilt": len(rebuilt),
        "rebuilt_objects": rebuilt,
        "output": output_stats(cproj / "build"),
    }


def main():
    if not Path("/usr/bin/time").exists():
        raise RuntimeError("GNU time is required")

    jobs = int(os.environ.get("BENCH_JOBS", os.cpu_count() or 1))
    impl.run(["make"], cwd=ROOT, quiet=True)

    with tempfile.TemporaryDirectory(prefix="c-sdl3-stats-") as directory:
        work = Path(directory)
        sdl = work / "SDL"
        impl.run(["git", "clone", "--quiet", impl.SDL_REPO, str(sdl)])
        impl.checkout(sdl, BASE)

        meta = work / "meta"
        impl.run(cmake_args(sdl, meta), quiet=True)
        cproj = work / "cproj"
        source_count, flags = impl.generate_build_c(meta, cproj, sdl)

        changes = change_stats(sdl)
        cmake_config = profile_cmake_config(sdl, work)
        ninja = ninja_stats(sdl, work, jobs)
        impl.checkout(sdl, BASE)
        c = c_stats(sdl, cproj, work, jobs)
        c_first_run = profile_c_first_run(cproj, work, jobs)

        result = {
            "machine": machine_stats(),
            "base": BASE,
            "change": CHANGE,
            "revision_override": "benchmark",
            "change_file": CHANGE_FILE,
            "change_stats": changes,
            "source_count": source_count,
            "shared_semantic_flags": len(flags),
            "jobs": jobs,
            "c": c,
            "cmake_ninja": ninja,
            "configuration": {
                "c_fresh_build_script_cache": c_first_run,
                "cmake_configure": cmake_config,
            },
        }
        result["standard"] = common.standard_contract(
            clean_c=c["clean"], clean_ninja=ninja["clean"],
            noop_c=c["noop"], noop_ninja=ninja["noop"],
            incremental_c=c["real_update"], incremental_ninja=ninja["real_update"],
            incremental_description=f"real SDL3 update {BASE[:8]}..{CHANGE[:8]}",
            rebuilt_tus={"c": c["translation_units_rebuilt"], "cmake_ninja": ninja["translation_units_rebuilt"]},
            runs={"clean": CLEAN_RUNS, "noop": NOOP_RUNS, "incremental": CHANGE_RUNS},
        )
        print("STATS_JSON=" + json.dumps(result, sort_keys=True))
        print(f"Rebuilt TUs: c={c['translation_units_rebuilt']} ninja={ninja['translation_units_rebuilt']}")
        print(f"Archive only: c={c['archive_only']['wall_ms']:.1f} ms ninja={ninja['archive_only']['wall_ms']:.1f} ms")
        print(f"Real update peak RSS: c={c['real_update']['max_rss_kb']} KiB ninja={ninja['real_update']['max_rss_kb']} KiB")


if __name__ == "__main__":
    main()
