"""Purpose-bound cryptographic operations for trusted in-process consumers."""

import base64
import hashlib
import hmac
from collections.abc import Callable
from dataclasses import dataclass

from cryptography.fernet import Fernet


@dataclass(frozen=True)
class SecretScope:
    encryption: bytes
    fingerprint: bytes


@dataclass(frozen=True)
class SecretPort:
    encrypt: Callable[[bytes], bytes]
    decrypt: Callable[[bytes], bytes]
    fingerprint: Callable[[bytes], str]


@dataclass(frozen=True)
class DigestPort:
    digest: Callable[[str], str]


def secret_port(master_key: str, scope: SecretScope) -> SecretPort:
    key = hmac.digest(master_key.encode(), scope.encryption, "sha256")
    cipher = Fernet(base64.urlsafe_b64encode(key))
    fingerprint_key = hmac.digest(key, scope.fingerprint, "sha256")
    return SecretPort(
        cipher.encrypt,
        cipher.decrypt,
        lambda content: hmac.digest(fingerprint_key, content, "sha256").hex(),
    )


def digest_port(secret: str) -> DigestPort:
    key = secret.encode()
    return DigestPort(lambda value: hmac.new(key, value.encode(), hashlib.sha256).hexdigest())
