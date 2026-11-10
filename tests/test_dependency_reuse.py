"""Existing dependency sources must be reused or rejected, never replaced."""
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import ii_file_provider as provider

project = Path(__file__).resolve().parents[1]
owner = project / "resources" / ".state" / ("dependency-guard-" + uuid.uuid4().hex[:12])
provider.mkdir(owner, parents=True, exist_ok=False)
try:
    source = owner / "resources" / "dependencies" / "demo"
    helper = project / "cmake" / "DependencySources.cmake"
    script = project / "tests" / "dependency_reuse.cmake"
    def call(mode):
        return subprocess.run([sys.argv[1], "-DROOT=" + str(owner), "-DHELPER=" + str(helper),
                               "-DMODE=" + mode, "-P", str(script)], capture_output=True, text=True)
    assert call("missing").returncode == 0
    provider.mkdir(source, parents=True, exist_ok=False)
    assert call("incomplete").returncode != 0
    assert source.is_dir()
    provider.mkdir(source / "include", parents=True, exist_ok=True)
    provider.write_text(source / "include" / "example.h", "fixture")
    assert call("valid").returncode == 0
    assert call("override").returncode == 0
    assert (source / "include" / "example.h").read_text() == "fixture"
finally:
    shutil.rmtree(owner)
