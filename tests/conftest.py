import fastapi.testclient
import pytest
import starlette.testclient
from entry_support import Entry


class EntryTestClient(starlette.testclient.TestClient):
    """API tests reach the internal app the way production does: via the entry."""

    def __init__(self, app, *args, **kwargs):
        super().__init__(Entry(app), *args, **kwargs)

    def scope(self, scope, session=""):
        """Issue a scope credential the way the entry's /access/scopes does."""
        return self.app.scope_token(scope, session)


fastapi.testclient.TestClient = EntryTestClient


@pytest.fixture
def accounts():
    """Account state the stand-in entry reports per session: session -> (status, email)."""
    from entry_support import ACCOUNTS

    yield ACCOUNTS
    ACCOUNTS.clear()


@pytest.fixture
def system_postgres():
    """Use the current platform package layout; each test owns a separate database cluster."""
    import platform
    import sys
    from pathlib import Path

    from asterion.runtime.desktop import pg_directory

    root = (
        Path("/usr")
        if sys.platform == "linux"
        else Path("/opt/homebrew" if platform.machine() == "arm64" else "/usr/local")
        / "opt/postgresql@17"
    )
    if not (pg_directory(root, "bindir") / "postgres").is_file():
        pytest.skip("system PostgreSQL 17 required")
    return root
