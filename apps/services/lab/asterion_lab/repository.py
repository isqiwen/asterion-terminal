"""The user's strategy directory, `research/` of the project by default.

Its strategies are the Strategy subclasses defined in the modules of its
`strategies/` directory, named `<module>.<class>`, and its factors the public
functions defined in the modules of `factors/`, named `<module>.<function>`;
modules import the directory's other code from its root, so a strategy can
use a factor. Inside a Git work tree, a strategy's version is the commit and
uncommitted changes of the directory are named by the digest of what differs
from it; outside one, the copy each run takes is the only record.

Strategy code is the user's: it is imported only in a child process, so that
a slow or broken module never holds up the service and every listing sees the
files as they are now. The child runs `main` in the repository and writes a
`Strategies` message (asterion/lab.proto) to standard output.
"""
import ast
from dataclasses import dataclass
import hashlib
import importlib
import importlib.util
import inspect
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import traceback

from asterion.lab_pb2 import Source, Strategies

from asterion_strategy import FIELDS, KINDS, Strategy

LISTING_SECONDS = 30
# What a strategy may import from outside its directory: what Trading's runtime
# provides. A factor runs only in Lab, which may import what Lab has.
RUNTIME_MODULES = frozenset(sys.stdlib_module_names) | {"numpy", "asterion_strategy"}
SNAPSHOT_BYTES = 16 * 1024 * 1024


@dataclass(frozen=True)
class Revision:
    commit: str
    changes: str  # digest of uncommitted changes; empty when the work tree is clean


def git(root: Path, *arguments) -> bytes:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True,
                                timeout=LISTING_SECONDS)
    except FileNotFoundError:
        raise ValueError("Git is not installed; a strategy repository needs it")
    if result.returncode != 0:
        raise ValueError(f"git {arguments[0]} failed in {root}: "
                         f"{result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def checked(root) -> Path:
    root = Path(root)
    if not root.is_absolute() or not root.is_dir():
        raise ValueError(f"strategy directory is not a directory: {root}")
    return root


def commit_of(root: Path) -> str:
    """The commit of the Git work tree holding the directory, or empty."""
    try:
        return git(root, "rev-parse", "--verify", "HEAD^{commit}").decode().strip()
    except ValueError:
        return ""


def revision(root: Path) -> Revision:
    commit = commit_of(root)
    if not commit:
        return Revision("", "")
    untracked = [path for path in git(root, "ls-files", "--others", "--exclude-standard", "-z")
                 .split(b"\0") if path]
    tracked = git(root, "diff", "HEAD", "--binary", "--no-ext-diff", "--no-textconv", "--", ".")
    if not tracked and not untracked:
        return Revision(commit, "")
    digest = hashlib.sha256(tracked)
    for path in sorted(untracked):
        digest.update(b"\0" + path + b"\0")
        digest.update((root / os.fsdecode(path)).read_bytes())
    return Revision(commit, digest.hexdigest())


def local(root: Path, name: str):
    """The directory's file of module `name`, "" for a directory without one
    (a namespace package), or None when the directory has neither."""
    path = "/".join(name.split("."))
    for candidate in (f"{path}.py", f"{path}/__init__.py"):
        if (root / candidate).is_file():
            return candidate
    return "" if (root / path).is_dir() else None


def trading_provides(name: str) -> bool:
    return name in RUNTIME_MODULES


def lab_provides(name: str) -> bool:
    return importlib.util.find_spec(name) is not None


def closure(root: Path, entry: str, provided=trading_provides) -> list:
    """`entry` and every module of the directory it imports, directly or not,
    with the packages holding them. Modules are read, never imported. An import
    neither in the directory nor `provided` (by top-level name) fails by name."""
    files, pending = set(), [entry]
    while pending:
        path = pending.pop()
        if path in files:
            continue
        files.add(path)
        folders = path.split("/")[:-1]
        package = ".".join(folders)
        for depth in range(1, len(folders) + 1):
            if found := local(root, ".".join(folders[:depth])):
                pending.append(found)
        for node in ast.walk(ast.parse((root / path).read_bytes(), filename=path)):
            if isinstance(node, ast.Import):
                required, optional = [alias.name for alias in node.names], []
            elif isinstance(node, ast.ImportFrom):
                anchor = package.split(".")[:len(folders) - node.level + 1] if node.level else []
                base = ".".join([*anchor, *([node.module] if node.module else [])])
                # `from a import b` names a module b or a value b of module a.
                required = [base] if base else []
                optional = [f"{base}.{alias.name}" if base else alias.name for alias in node.names]
            else:
                continue
            for name in required:
                found = local(root, name)
                if found is None and not provided(name.split(".")[0]):
                    raise ValueError(f"{path} imports {name}, which is neither in the strategy "
                                     "directory nor in the runtime")
                if found:
                    pending.append(found)
            pending.extend(found for found in map(lambda name: local(root, name), optional) if found)
    return sorted(files)


def snapshot(root: Path, destination: Path, entry: str, provided=trading_provides) -> Revision:
    """Copies `entry` and the modules it needs (closure) to `destination` for
    an admitted run, which then never reads the repository itself."""
    root = checked(root)
    taken = revision(root)
    files = closure(root, entry, provided)
    if sum((root / name).stat().st_size for name in files) > SNAPSHOT_BYTES:
        raise ValueError(f"the strategy's Python files exceed {SNAPSHOT_BYTES >> 20} MiB")
    destination.mkdir(parents=True, exist_ok=True)
    for name in files:
        (destination / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, destination / name)
    return taken


def import_modules(root: Path, directory="strategies"):
    """Each module of a directory by name, or the error importing it raised."""
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    for path in sorted((root / directory).glob("*.py")):
        if path.stem.startswith("_"):
            continue
        try:
            module, error = importlib.import_module(f"{directory}.{path.stem}"), None
        except BaseException:
            module, error = None, traceback.format_exc(limit=-3).strip()
        yield path.stem, module, error


def defined(module) -> list:
    return [value for _, value in inspect.getmembers(module, inspect.isclass)
            if issubclass(value, Strategy) and value is not Strategy
            and value.__module__ == module.__name__]


def load(root: Path, identity: str, values: dict) -> Strategy:
    """The strategy `<module>.<class>` of a repository or its snapshot, with
    its parameters set, in the process that runs it."""
    name, _, class_name = identity.partition(".")
    for module_name, module, error in import_modules(root):
        if module_name != name:
            continue
        if error:
            raise ValueError(f"strategy module {name} failed to import:\n{error}")
        for cls in defined(module):
            if cls.__name__ == class_name:
                return cls(**values)
    raise ValueError(f"no strategy {identity} in {root}")


def factors(module) -> list:
    """The module's own public functions, each with its bar inputs and its
    settings with their defaults."""
    found = []
    for name, function in inspect.getmembers(module, inspect.isfunction):
        if name.startswith("_") or function.__module__ != module.__name__:
            continue
        inputs, settings = [], {}
        for parameter in inspect.signature(function).parameters.values():
            where = f"factor {module.__name__}.{name}: parameter {parameter.name}"
            if parameter.kind not in (parameter.POSITIONAL_OR_KEYWORD, parameter.KEYWORD_ONLY):
                raise TypeError(f"{where} is variadic or positional only")
            if parameter.name in FIELDS:
                inputs.append(parameter.name)
            elif type(parameter.default) in KINDS:
                settings[parameter.name] = parameter.default
            else:
                raise TypeError(f"{where} is no bar field ({', '.join(FIELDS)}) and its default "
                                "is not a bool, int, float or str")
        if not inputs:
            raise TypeError(f"factor {module.__name__}.{name} reads no bar field")
        found.append((name, function, inputs, settings))
    return found


def factor(root: Path, identity: str):
    """The repository's factor `<module>.<function>` with its inputs and settings."""
    root = checked(root)
    name, _, function_name = identity.partition(".")
    for module_name, module, error in import_modules(root, "factors"):
        if module_name != name:
            continue
        if error:
            raise ValueError(f"factor module {name} failed to import:\n{error}")
        for found in factors(module):
            if found[0] == function_name:
                return found[1:]
    raise ValueError(f"no factor {identity} in {root}")


def describe(root: Path) -> Strategies:
    reply = Strategies(repository=str(root))
    root = checked(root)
    current = revision(root)
    reply.commit, reply.changes = current.commit, current.changes
    for name, module, error in import_modules(root):
        if error:
            reply.errors.add(module=name, traceback=error)
            continue
        for cls in defined(module):
            entry = reply.strategies.add(id=f"{name}.{cls.__name__}",
                                         description=inspect.getdoc(cls) or "")
            for parameter, value in cls.params.items():
                item = entry.parameters.add(name=parameter)
                setattr(item.default, VALUE_FIELDS[type(value)], value)
    for name, module, error in import_modules(root, "factors"):
        try:
            if error:
                raise ValueError(error)
            found = factors(module)
        except (TypeError, ValueError) as failure:
            reply.errors.add(module=f"factors/{name}", traceback=str(failure))
            continue
        for function_name, function, inputs, settings in found:
            entry = reply.factors.add(id=f"{name}.{function_name}",
                                      description=inspect.getdoc(function) or "", inputs=inputs)
            for parameter, value in settings.items():
                item = entry.parameters.add(name=parameter)
                setattr(item.default, VALUE_FIELDS[type(value)], value)
    return reply


VALUE_FIELDS = {bool: "flag", int: "integer", float: "number", str: "text"}


def child(module: str) -> list:
    """The command that runs `module`.main() in a child process that imports
    from where this process does."""
    # The child runs elsewhere: relative entries are made absolute.
    path = [os.path.abspath(entry) for entry in sys.path]
    start = f"import sys; sys.path[:] = {path!r}; from {module} import main; main()"
    return [sys.executable, "-I", "-B", "-X", "faulthandler", "-c", start]


MODULE = re.compile(r"(strategies|factors)/[A-Za-z_][A-Za-z0-9_]*\.py")


def module(root: Path, path: str) -> Path:
    if not MODULE.fullmatch(path):
        raise ValueError(f"a module is strategies/<name>.py or factors/<name>.py: {path}")
    return checked(root) / path


def read_source(root: Path, path: str) -> Source:
    file = module(root, path)
    if not file.is_file():
        raise ValueError(f"no module {path}")
    return Source(path=path, content=file.read_text(), read_only=not os.access(file, os.W_OK))


def write_source(root: Path, path: str, content: str, create: bool) -> Source:
    # Not at the top: the listing child imports this module and need not load DuckDB.
    from asterion_lab.warehouse import write_durably
    file = module(root, path)
    if file.exists() and (create or not os.access(file, os.W_OK)):
        raise ValueError(f"a module of this name already exists: {path}" if create
                         else f"a built-in module cannot be changed: {path}")
    write_durably(file, content.encode())
    return read_source(root, path)


def delete_source(root: Path, path: str):
    file = module(root, path)
    if not file.is_file():
        raise ValueError(f"no module {path}")
    if not os.access(file, os.W_OK):
        raise ValueError(f"a built-in module cannot be deleted: {path}")
    file.unlink()


def list_strategies(root: str) -> Strategies:
    """The repository's strategies, imported in a child process."""
    if not Path(root).is_absolute() or not Path(root).is_dir():
        raise ValueError(f"strategy repository is not a directory: {root}")
    try:
        result = subprocess.run([*child("asterion_lab.repository"), root],
                                capture_output=True, timeout=LISTING_SECONDS, cwd=root)
    except subprocess.TimeoutExpired:
        raise ValueError(f"strategy modules took longer than {LISTING_SECONDS} s to import")
    if result.returncode != 0:
        lines = result.stderr.decode(errors="replace").strip().splitlines()
        raise ValueError(lines[-1] if lines else f"strategy listing exited with {result.returncode}")
    reply = Strategies()
    reply.ParseFromString(result.stdout)
    return reply


def main():
    # What strategy code prints goes to standard error; standard output carries the reply.
    output = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    try:
        reply = describe(Path(sys.argv[1]))
    except ValueError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
    output.write(reply.SerializeToString())
    output.flush()
