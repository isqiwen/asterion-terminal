"""Source-only test doubles explicitly do not offer the scan capability."""


def unsupported_scan(_):
    raise AssertionError("This source-only fixture must not be scanned")
