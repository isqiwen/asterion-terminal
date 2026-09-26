"""Opaque handles for host-approved Rust cryptographic operations."""

from collections.abc import Callable
from dataclasses import dataclass

from . import _native
from ._native import SecretError

__all__ = ["SecretError", "SecretPort", "SecretScope", "secret_port"]


@dataclass(frozen=True)
class SecretScope:
    encryption: bytes
    fingerprint: bytes


@dataclass(frozen=True)
class SecretPort:
    encrypt: Callable[[bytes], bytes]
    decrypt: Callable[[bytes], bytes]
    fingerprint: Callable[[bytes], str]


def secret_port(master_key: str, scope: SecretScope) -> SecretPort:
    handle = _native.SecretHandle(master_key, scope.encryption, scope.fingerprint)
    return SecretPort(handle.encrypt, handle.decrypt, handle.fingerprint)
