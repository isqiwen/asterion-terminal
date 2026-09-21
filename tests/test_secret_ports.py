from dataclasses import fields

import pytest
from cryptography.fernet import InvalidToken

from asterion.platform.secrets import DigestPort, SecretPort, SecretScope, digest_port, secret_port


def test_secret_ports_bind_ciphertext_and_fingerprints_to_the_approved_purpose():
    scope = SecretScope(b"fixture.credentials", b"fixture.snapshot")
    first = secret_port("fixture-key", scope)
    second = secret_port("fixture-key", SecretScope(b"other.credentials", b"fixture.snapshot"))
    wrong_key = secret_port("other-key", scope)
    content = b'{"credential":"sensitive"}'
    encrypted = first.encrypt(content)
    assert first.decrypt(encrypted) == content
    assert secret_port("fixture-key", scope).decrypt(encrypted) == content
    assert content not in encrypted
    for port in (second, wrong_key):
        with pytest.raises(InvalidToken):
            port.decrypt(encrypted)
        assert port.fingerprint(content) != first.fingerprint(content)
    assert {field.name for field in fields(SecretPort)} == {"encrypt", "decrypt", "fingerprint"}


def test_tampered_credential_ciphertext_is_rejected():
    port = secret_port("fixture-key", SecretScope(b"fixture.credentials", b"fixture.snapshot"))
    encrypted = port.encrypt(b"credential")
    damaged = bytearray(encrypted)
    damaged[len(damaged) // 2] ^= 1
    with pytest.raises(InvalidToken):
        port.decrypt(bytes(damaged))


def test_identity_digest_port_only_exposes_the_required_operation():
    signer = digest_port("account-key")
    assert signer.digest("session") == digest_port("account-key").digest("session")
    assert signer.digest("session") != signer.digest("other-session")
    assert signer.digest("session") != digest_port("other-key").digest("session")
    assert {field.name for field in fields(DigestPort)} == {"digest"}
