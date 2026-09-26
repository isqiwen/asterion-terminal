"""Create test credentials through the current revisioned configuration contract."""


def set_token(service, provider, token):
    state = service.sources.state(provider)
    return service.sources.apply(
        provider,
        state["revision"],
        values=state["values"],
        secrets={"token": token or None},
    )


def saved_values(service, owner, provider=None):
    """The runnable values saved for `owner`, read back through a fixed snapshot."""
    provider = provider or owner
    with service.engine.connect() as conn:
        fixed = service.sources.fix_for_task(conn, owner, provider)["configuration"]
    spec = service.registry.get(provider).manifest.configuration.model_dump(mode="json")
    return service.credentials.resolve(fixed, owner, spec)
