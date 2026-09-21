"""Deterministic registration for trusted, in-process contributions."""

from collections.abc import Iterable


class Registry[T]:
    def __init__(self, entries: Iterable[tuple[str, T]] = ()):
        self._entries: dict[str, T] = {}
        for identifier, value in entries:
            self.register(identifier, value)

    def register(self, identifier: str, value: T) -> None:
        if not identifier or identifier.strip() != identifier:
            raise ValueError("Contribution ID must be nonempty and have no surrounding whitespace")
        if identifier in self._entries:
            raise ValueError(f"Duplicate contribution: {identifier}")
        self._entries[identifier] = value

    def get(self, identifier: str) -> T:
        return self._entries[identifier]

    def all(self) -> tuple[T, ...]:
        return tuple(self._entries.values())
