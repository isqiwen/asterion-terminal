"""Keep core dependencies downward and domain observations independent of JSON."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
layers = {"foundation": 0, "kernel": 1, "domain": 2}
errors = []
for tree in (root / "core/include/asterion", root / "core/src"):
    for layer, rank in layers.items():
        for path in sorted((tree / layer).rglob("*")):
            if path.suffix not in (".hpp", ".cpp"):
                continue
            source = path.read_text()
            for name in re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', source):
                parts = name.split("/")
                if (parts[0] in ("apps", "plugins") or
                    (parts[0] == "asterion" and len(parts) > 1 and
                     (parts[1] not in layers or layers[parts[1]] > rank) and
                     not (layer == "kernel" and name == "asterion/plugin/abi.h"))):
                    errors.append(f"{path.relative_to(root)}: forbidden dependency {name}")
                if layer == "domain" and name in (
                    "asterion/foundation/serialization.hpp", "nlohmann/json.hpp",
                    "nlohmann/json_fwd.hpp",
                ):
                    errors.append(f"{path.relative_to(root)}: JSON belongs at a boundary")
            code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', '', source, flags=re.S)
            if layer == "domain" and re.search(r'\b(?:Json|nlohmann)\b', code):
                errors.append(f"{path.relative_to(root)}: domain API must use domain types")
if errors:
    raise SystemExit("\n".join(errors))
print("Core source boundaries passed")
