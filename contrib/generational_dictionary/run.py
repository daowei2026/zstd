#!/usr/bin/env python3
"""Build this prototype without writing generated files into the checkout."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import time

SOURCE = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["build", "test", "bench", "regression", "adapt", "network"])
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--variant", choices=["baseline", "prototype", "sanitize"], required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--ar", default="ar")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--partition-bytes", type=int, default=30000000)
    parser.add_argument("--frames", type=int, default=10000)
    parser.add_argument("--baseline-level", type=int, choices=[0, 3, 9], default=3)
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--static", action="store_true")
    parser.add_argument("--block-bytes", type=int, choices=[4080, 4096], default=4096)
    args = parser.parse_args()
    if not args.output_root.is_absolute():
        parser.error("output-root must be absolute")
    root = args.output_root.resolve()
    if root == SOURCE or SOURCE in root.parents:
        parser.error("output-root must be outside the source checkout")
    out = root / "artifacts" / args.variant
    logs = root / "evidence" / (args.variant + "-" + args.action + "-" + str(time.time_ns()))
    tmp = root / "tmp" / args.variant
    for directory in (out, logs, tmp):
        directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(tmp))
    flags = ["-std=c99", "-DZSTD_DISABLE_ASM=1", "-DZSTD_LEGACY_SUPPORT=0",
             "-DZSTD_MULTITHREAD", "-DGD_BLOCK_SIZE=" + str(args.block_bytes), "-I" + str(SOURCE / "lib"), "-pthread"]
    if args.variant == "sanitize":
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    else:
        flags += ["-O3"]
    if args.static:
        flags += ["-static"]
    if args.action != "build":
        library = out / "libzstd.a"
        if not library.is_file() or (args.action == "test" and args.variant == "baseline"):
            parser.error("build the matching library before testing")
        executable = out / ("generational_" + args.action)
        names = ["dictionary.c", args.action + ".c"]
        arguments = []
        if args.action == "bench":
            arguments = [str(args.partition_bytes), str(args.frames), str(args.baseline_level)]
            if args.variant == "baseline":
                names = ["bench.c"]
                flags += ["-DGD_BASELINE=1"]
        inputs = [SOURCE / "contrib/generational_dictionary" / name for name in names]
        if args.action == "regression":
            inputs = [SOURCE / name for name in ("tests/fuzzer.c", "programs/datagen.c", "programs/util.c", "programs/timefn.c")]
            flags += ["-I" + str(SOURCE / "programs"), "-I" + str(SOURCE / "lib/common"), "-Wno-deprecated-declarations"]
            arguments = ["-i1000", "-s71237"]
        command = [args.cc, *flags, "-Wall", "-Wextra", "-Werror", *(str(p) for p in inputs),
                   str(library), "-o", str(executable)]
        results = []
        operations = [("compile", command)]
        if not args.compile_only and args.action != "network":
            operations.append((args.action, [str(executable), *arguments]))
        for name, argv in operations:
            started = time.time()
            with (logs / (name + ".log")).open("w") as stream:
                result = subprocess.run(argv, cwd=SOURCE, env=env, stdout=stream, stderr=subprocess.STDOUT)
            results.append({"command": argv, "start_unix": started, "end_unix": time.time(), "returncode": result.returncode})
            print((logs / (name + ".log")).read_text()[-12000:], end="", flush=True)
            if result.returncode:
                break
        if args.action == "network" and not args.compile_only and not results[-1]["returncode"]:
            server_command = [str(executable), "serve", "127.0.0.1", "43967"]
            client_command = [str(executable), "send", "127.0.0.1", "43967"]
            with (logs / "server.log").open("w") as stream:
                server = subprocess.Popen(server_command, cwd=SOURCE, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                started = time.time()
                try:
                    ready = server.stdout.readline()
                    stream.write(ready)
                    if not ready.startswith("READY"):
                        raise RuntimeError("UDP fixture did not start: " + ready)
                    with (logs / "client.log").open("w") as client_log:
                        client = subprocess.run(client_command, cwd=SOURCE, env=env, stdout=client_log, stderr=subprocess.STDOUT, timeout=160)
                    results.append({"command": client_command, "start_unix": started, "end_unix": time.time(), "returncode": client.returncode})
                    if client.returncode:
                        server.terminate()
                    rest, _ = server.communicate(timeout=5)
                    stream.write(rest)
                    results.append({"command": server_command, "start_unix": started, "end_unix": time.time(), "returncode": server.returncode})
                finally:
                    if server.poll() is None:
                        server.kill(); server.wait()
            print((logs / "server.log").read_text() + (logs / "client.log").read_text(), end="", flush=True)
        (logs / "manifest.json").write_text(json.dumps({
            "results": results, "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip(),
            "inputs_sha256": {str(p.relative_to(SOURCE)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
            "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        }, indent=2) + "\n")
        failed = any(r["returncode"] for r in results)
        print(json.dumps({"returncode": int(failed), "logs": str(logs)}), flush=True)
        return int(failed)
    build_source = SOURCE
    if args.variant == "baseline":
        build_source = root / "baseline-source"
        build_source.mkdir(exist_ok=True)
        archive = tmp / "baseline.tar"
        with archive.open("wb") as stream:
            subprocess.run(["git", "archive", "f8745da6ff1ad1e7bab384bd1f9d742439278e99", "lib"],
                           cwd=SOURCE, stdout=stream, check=True)
        with tarfile.open(archive) as packed:
            packed.extractall(build_source, filter="data")
        flags = [flag if flag != "-I" + str(SOURCE / "lib") else "-I" + str(build_source / "lib") for flag in flags]
    sources = sorted(p for part in ("common", "compress", "decompress", "dictBuilder")
                     for p in (build_source / "lib" / part).glob("*.c"))
    jobs = [(p, out / (p.parent.name + "_" + p.stem + ".o")) for p in sources]
    start = time.time()

    def compile_one(job):
        src, obj = job
        command = [args.cc, *flags, "-c", str(src), "-o", str(obj)]
        with (logs / (obj.stem + ".log")).open("w") as stream:
            result = subprocess.run(command, cwd=SOURCE, env=env, stdout=stream, stderr=subprocess.STDOUT)
        return {"command": command, "returncode": result.returncode}

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(compile_one, jobs))
    failed = [r for r in results if r["returncode"]]
    if not failed:
        command = [args.ar, "rcs", str(out / "libzstd.a"), *(str(obj) for _, obj in jobs)]
        result = subprocess.run(command, cwd=SOURCE, env=env, check=False)
        results.append({"command": command, "returncode": result.returncode})
        if result.returncode:
            failed.append(results[-1])
    manifest = {
        "start_unix": start, "end_unix": time.time(), "source": str(SOURCE),
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip(),
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "library_source": str(build_source),
        "source_sha256": {str(p.relative_to(build_source)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sources + sorted((build_source / "lib").rglob("*.h"))},
        "results": results,
    }
    (logs / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"variant": args.variant, "compiled": len(jobs), "failures": len(failed),
                      "seconds": round(time.time() - start, 3), "logs": str(logs)}), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
