"""Check that generated test state is confined and removed."""
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import ii_file_provider as provider

root = Path(__file__).resolve().parents[1]
owner = root / "resources" / ".state" / ("guard-" + uuid.uuid4().hex[:12])
provider.mkdir(owner, parents=True, exist_ok=False)
try:
    state = owner / "state"
    output = state / "tmp"
    script = root / "tests" / "test_output_fixture.cmake"
    def call(action, target):
        return subprocess.run([sys.argv[1], "-DRESOURCE_ROOT=" + str(state),
                               "-DOUTPUT_DIR=" + str(target), "-DACTION=" + action,
                               "-P", str(script)], capture_output=True, text=True)
    assert call("setup", owner / "outside").returncode != 0
    assert not (owner / "outside").exists()
    assert call("setup", output).returncode == 0
    assert output.is_dir()
    assert call("cleanup", output).returncode == 0
    assert not output.exists()
finally:
    shutil.rmtree(owner)
