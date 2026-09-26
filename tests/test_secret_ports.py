from dataclasses import fields

import pytest
from asterion_bindings.secrets import SecretError, SecretPort, SecretScope, secret_port


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
        with pytest.raises(SecretError):
            port.decrypt(encrypted)
        assert port.fingerprint(content) != first.fingerprint(content)
    assert {field.name for field in fields(SecretPort)} == {"encrypt", "decrypt", "fingerprint"}


def test_tampered_credential_ciphertext_is_rejected():
    port = secret_port("fixture-key", SecretScope(b"fixture.credentials", b"fixture.snapshot"))
    encrypted = port.encrypt(b"credential")
    damaged = bytearray(encrypted)
    damaged[len(damaged) // 2] ^= 1
    with pytest.raises(SecretError):
        port.decrypt(bytes(damaged))


def test_fernet_standard_fixture_and_fingerprint_are_stable():
    # Public fixture produced independently with the Fernet specification and HMAC-SHA256.
    port = secret_port("fixture-key", SecretScope(b"fixture.credentials", b"fixture.snapshot"))
    content = b"current standard cipher fixture"
    token = (
        b"gAAAAABm7OCAJ0XmBoAfZDX0j10uiRuHgmsSJGMe5H6CIx_MSTHylZn8J6LX3gQ-"
        b"SB9tOvcluXBsggRQ0hw6OZt9_QA9wxQmnRM4c3TOFZ4va8Fw5uQ-TlY="
    )
    assert port.decrypt(token) == content
    assert port.fingerprint(content) == (
        "9fed7c1638840c55b6180d51a8f9772876c1791d3c0245bf1c8eb82e406c7b5b"
    )
    assert port.encrypt(content) != port.encrypt(content)
