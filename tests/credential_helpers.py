"""Test assembly of approved data cryptography; no plaintext keys enter services."""

from asterion_bindings.data_sources import SourceCredentials


def provider_secrets(key, root):
    return SourceCredentials(key, root)
