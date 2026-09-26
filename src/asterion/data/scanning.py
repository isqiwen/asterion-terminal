"""Application metadata lookup and ownership for the Rust L2 daily scanner."""

from asterion_bindings.data_scan import ScanBatches, open_scan
from sqlalchemy import select

from asterion.data.library import versions
from asterion.data.scan_public import ScanRequest, ScanResult


def scan(library, request: ScanRequest) -> ScanResult:
    # A fixed immutable version is captured within the current read grant. The
    # lazy scanner retains its resource lifetime, never this database transaction.
    with library.engine.connect() as connection:
        row = (
            connection.execute(select(versions).where(versions.c.id == request.version_id))
            .mappings()
            .first()
        )
        if row is None:
            raise KeyError(request.version_id)
        version = dict(row)
    scanner = open_scan(library.root, library.engine._scope, version, request)
    return ScanResult(library.public(version), ScanBatches(scanner))
