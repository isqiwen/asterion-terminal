"""Installations for process tests, as docs/architecture.md (安装目录) lays them out."""
import os
from pathlib import Path


def install(root, program, executable) -> Path:
    """The build's `executable` placed in <root>/<program>/bin through a link: it
    finds its config/, logs/ and data/ there. Returns the link to start."""
    link = Path(root) / program / "bin" / Path(executable).name
    link.parent.mkdir(parents=True, exist_ok=True)
    (Path(root) / program / "data").mkdir(parents=True, exist_ok=True)
    if not link.is_symlink():
        link.symlink_to(os.path.abspath(executable))
    return link


def install_agent(root, agent) -> Path:
    """The build's Agent in <root>/agent: it keeps its state in <root>/agent/data
    and installs the services under <root>."""
    return install(root, "agent", agent)
