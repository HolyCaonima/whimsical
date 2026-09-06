"""Check that the external snapshot still matches every original upstream file."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / "external/AI4AnimationPy.provenance.json").read_text(encoding="utf-8"))
vendor = root / "external/AI4AnimationPy"
for name, expected in manifest["files"].items():
    actual = hashlib.sha256((vendor / name).read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit("Modified upstream file: " + name)
print(f'Verified {len(manifest["files"])} original files at {manifest["commit"]}')
