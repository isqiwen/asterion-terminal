"""Publish a fully checked generated resource tree without retaining obsolete files."""
import http.client
import os
from pathlib import Path
import shutil
import time
import urllib.error
import urllib.request

TRANSIENT_HTTP = frozenset({429, 500, 502, 503, 504})


def download_file(url: str, destination: Path, *, attempts: int = 4, backoff: float = 2.0):
    """Download url atomically to destination, retrying transient GitHub/CDN failures."""
    temp = destination.with_name(destination.name + ".tmp")
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=60) as response, temp.open("wb") as output:
                shutil.copyfileobj(response, output)
            temp.replace(destination)
            return
        except urllib.error.HTTPError as error:
            if error.code not in TRANSIENT_HTTP or attempt + 1 == attempts:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead):
            if attempt + 1 == attempts:
                raise
        time.sleep(backoff * (2**attempt))


def publish_tree(prepared: Path, destination: Path):
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError("Generated resource destination must be a real directory")
    # Only call for generated packaging directories. The complete replacement
    # has already been built and checked; failure never publishes a partial set.
    if destination.exists():
        shutil.rmtree(destination)
    os.replace(prepared, destination)
