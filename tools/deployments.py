"""Track only this workbench's uploads; never infer ownership from a prefix."""
import json
from pathlib import Path

LEDGER = Path(__file__).resolve().parents[1] / "build/payload-deployments.json"

def load():
    return json.loads(LEDGER.read_text()) if LEDGER.exists() else []

def record(base, stem, sha256, path):
    entries = load()
    entries = [entry for entry in entries if not (entry["manager"] == base and entry["path"] == path)]
    entries.append({"manager": base, "stem": stem, "sha256": sha256, "path": path})
    LEDGER.parent.mkdir(parents=True, exist_ok=True)
    temporary = LEDGER.with_suffix(".tmp")
    temporary.write_text(json.dumps(entries, indent=2) + "\n")
    temporary.replace(LEDGER)

def superseded(entries, base, stem, current_path, inventory):
    """Only recorded paths, still installed and unambiguous, can be deleted."""
    selected = []
    for entry in entries:
        path = entry["path"]
        expected = f"{stem}-{entry['sha256'][:12]}.elf"
        if (entry["manager"] != base or entry["stem"] != stem or path == current_path
                or path not in inventory or Path(path).name != expected):
            continue
        if sum(Path(item).name == expected for item in inventory) == 1:
            selected.append(path)
    return selected
