"""The user's strategy repository: a Git work tree of their own (decision D5).

Its strategies are the Strategy subclasses defined in the modules of its
`strategies/` directory, named `<module>.<class>`; modules there import the
repository's other code from its root. A strategy's version is the commit;
uncommitted changes are named by the digest of what differs from it.

Strategy code is the user's: it is imported only in a child process, so that
a slow or broken module never holds up the service and every listing sees the
files as they are now. The child runs `main` in the repository and writes a
`Strategies` message (asterion/lab.proto) to standard output.
"""
from dataclasses import dataclass
import hashlib
import importlib
import inspect
import os
from pathlib import Path
import subprocess
import sys
import traceback

from asterion.lab_pb2 import Strategies

from asterion_lab.strategy import KINDS, Strategy

LISTING_SECONDS = 30


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
        raise ValueError(f"strategy repository is not a directory: {root}")
    top = git(root, "rev-parse", "--show-toplevel").decode().strip()
    if Path(top).resolve() != root.resolve():
        raise ValueError(f"strategy repository is not the top of a Git work tree: {root}")
    return root


def revision(root: Path) -> Revision:
    try:
        commit = git(root, "rev-parse", "--verify", "HEAD^{commit}").decode().strip()
    except ValueError:
        raise ValueError(f"strategy repository has no commit yet: {root}")
    untracked = [path for path in git(root, "ls-files", "--others", "--exclude-standard", "-z")
                 .split(b"\0") if path]
    tracked = git(root, "diff", "HEAD", "--binary", "--no-ext-diff", "--no-textconv")
    if not tracked and not untracked:
        return Revision(commit, "")
    digest = hashlib.sha256(tracked)
    for path in sorted(untracked):
        digest.update(b"\0" + path + b"\0")
        digest.update((root / os.fsdecode(path)).read_bytes())
    return Revision(commit, digest.hexdigest())


def import_modules(root: Path):
    """Each module of strategies/ by name, or the error importing it raised."""
    sys.path.insert(0, str(root))
    for path in sorted((root / "strategies").glob("*.py")):
        if path.stem.startswith("_"):
            continue
        try:
            module, error = importlib.import_module(f"strategies.{path.stem}"), None
        except BaseException:
            module, error = None, traceback.format_exc(limit=-3).strip()
        yield path.stem, module, error


def defined(module) -> list:
    return [value for _, value in inspect.getmembers(module, inspect.isclass)
            if issubclass(value, Strategy) and value is not Strategy
            and value.__module__ == module.__name__]


def load(root: Path, identity: str, values: dict) -> Strategy:
    """The repository's strategy `<module>.<class>` with its parameters set,
    in the process that runs it."""
    root = checked(root)
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


def describe(root: Path) -> Strategies:
    reply = Strategies(repository=str(root))
    root = checked(root)
    current = revision(root)
    reply.commit, reply.changes = current.commit, current.changes
    for name, module, error in import_modules(root):
        if error:
            reply.errors.add(module=name, message=error)
            continue
        for cls in defined(module):
            entry = reply.strategies.add(id=f"{name}.{cls.__name__}",
                                         description=inspect.getdoc(cls) or "")
            for parameter, value in cls.params.items():
                item = entry.parameters.add(name=parameter, kind=KINDS[type(value)])
                setattr(item.default, VALUE_FIELDS[type(value)], value)
    return reply


VALUE_FIELDS = {bool: "flag", int: "integer", float: "number", str: "text"}


def child(module: str) -> list:
    """The command that runs `module`.main() in a child process that imports
    from where this process does."""
    # The child runs elsewhere: relative entries are made absolute.
    path = [os.path.abspath(entry) for entry in sys.path]
    start = f"import sys; sys.path[:] = {path!r}; from {module} import main; main()"
    return [sys.executable, "-I", "-B", "-c", start]


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
