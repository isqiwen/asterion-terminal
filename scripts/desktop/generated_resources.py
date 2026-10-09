"""Publish a fully checked generated resource tree without retaining obsolete files."""
import os
from pathlib import Path
import shutil


def publish_tree(prepared: Path, destination: Path):
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError("Generated resource destination must be a real directory")
    # Only call for generated packaging directories. The complete replacement
    # has already been built and checked; failure never publishes a partial set.
    if destination.exists():
        shutil.rmtree(destination)
    os.replace(prepared, destination)
