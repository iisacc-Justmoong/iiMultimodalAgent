import json
import pathlib
import subprocess
import sys
import tempfile

cli = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="iimagent-rag-") as temporary:
    directory = pathlib.Path(temporary)
    store = directory / "corpus.json"
    source = directory / "source.json"
    source.write_text(json.dumps({
        "id": "creator/canvas", "title": "Canvas", "uri": "society://canvas/42",
        "revision": "rev-7", "kind": "document",
        "pages": [{"locator": "canvas:42", "text": "A creator retains the original painting."}],
    }), encoding="utf-8")

    def run(*args, ok=True):
        result = subprocess.run([cli, *map(str, args)], capture_output=True, text=True, timeout=20)
        assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
        return json.loads(result.stdout) if ok else result.stderr

    assert run("put", store, source)["revision"] == "rev-7"
    hits = run("search", store, "original painting")["hits"]
    assert len(hits) == 1 and hits[0]["source_id"] == "creator/canvas"
    assert hits[0]["uri"] == "society://canvas/42" and hits[0]["revision"] == "rev-7"
    assert run("remove", store, "creator/canvas")["removed"]
    assert not run("search", store, "painting")["hits"]
    assert not run("remove", store, "creator/canvas")["removed"]
    run("search", store, "painting", "0", ok=False)
