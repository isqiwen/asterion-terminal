"""Test assembly of approved data cryptography; no plaintext keys enter services."""

from asterion.data.public import CREDENTIAL_SCOPE
from asterion.platform.secrets import secret_port


def provider_secrets(key):
    return secret_port(key, CREDENTIAL_SCOPE)
