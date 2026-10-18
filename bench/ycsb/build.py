from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "bench" / "ycsb"
BUILD = ROOT / "build" / "ycsb"


def java_home() -> Path:
    configured = os.environ.get("JAVA_HOME")
    if configured:
        return Path(configured).resolve()
    if platform.system() == "Darwin":
        return Path(
            subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "21"], text=True
            ).strip()
        )
    compiler = shutil.which("javac")
    if compiler is None:
        raise RuntimeError("Java 21 JDK is required")
    return Path(compiler).resolve().parents[1]


def java_command(main: str, *arguments: str) -> list[str]:
    config = json.loads((BUILD / "build.json").read_text())
    return [
        config["java"],
        "-Xms256m",
        "-Xmx1g",
        "-Dycsb.native=" + config["native"],
        "-cp",
        config["classpath"],
        main,
        *arguments,
    ]


def build() -> dict[str, object]:
    lock = json.loads((SOURCE / "dependencies.lock.json").read_text())
    deps = BUILD / "deps"
    deps.mkdir(parents=True, exist_ok=True)
    for artifact in lock["artifacts"]:
        if artifact["kind"] == "jar":
            destination = deps / artifact["name"]
            if not destination.exists():
                with urllib.request.urlopen(artifact["url"], timeout=60) as response:
                    data = response.read()
                if hashlib.sha256(data).hexdigest() != artifact["sha256"]:
                    raise RuntimeError(
                        "download checksum mismatch: " + artifact["name"]
                    )
                destination.write_bytes(data)
        else:
            destination = (
                SOURCE
                / ("workloads" if artifact["name"].startswith("workload") else "")
                / artifact["name"]
            )
        if hashlib.sha256(destination.read_bytes()).hexdigest() != artifact["sha256"]:
            raise RuntimeError("checksum mismatch: " + str(destination))
    home = java_home()
    java = str(home / "bin" / "java")
    java_version = subprocess.check_output(
        [java, "-version"], stderr=subprocess.STDOUT, text=True
    )
    if 'version "21.' not in java_version:
        raise RuntimeError("Java 21 JDK is required: " + java_version)
    subprocess.run(["make", "all"], cwd=ROOT, check=True)
    suffix = "dylib" if platform.system() == "Darwin" else "so"
    native = BUILD / ("libycsb." + suffix)
    command = [
        "cc",
        "-O2",
        "-g",
        "-fPIC",
        "-shared",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(ROOT / "include"),
        "-I" + str(home / "include"),
        "-I" + str(home / "include" / ("darwin" if suffix == "dylib" else "linux")),
        str(SOURCE / "native" / "bridge.c"),
        "-L" + str(ROOT / "build" / "lib"),
        "-ltotedb",
        "-lsqlite3",
        "-Wl,-rpath," + str(ROOT / "build" / "lib"),
        "-o",
        str(native),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    classes = BUILD / "classes"
    classes.mkdir(exist_ok=True)
    jars = sorted(str(path) for path in deps.glob("*.jar"))
    classpath = os.pathsep.join([str(classes), *jars])
    subprocess.run(
        [
            str(home / "bin" / "javac"),
            "-Xlint:all",
            "-Werror",
            "-cp",
            classpath,
            "-d",
            str(classes),
            *sorted(str(path) for path in (SOURCE / "java").rglob("*.java")),
        ],
        check=True,
    )
    config = {
        "java": java,
        "java_version": java_version,
        "native": str(native),
        "classpath": classpath,
        "native_build_command": command,
        "dependency_lock": lock,
        "compiler": subprocess.check_output(["cc", "--version"], text=True),
    }
    (BUILD / "build.json").write_text(json.dumps(config, indent=2) + "\n")
    return config
