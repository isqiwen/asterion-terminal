"""Create test credentials through the current revisioned configuration contract."""

from pydantic import SecretStr

from asterion.data.configuration import ConfigurationUpdate


def set_token(service, provider, token):
    state = service.configuration.state(provider)
    return service.configuration.apply(
        provider,
        ConfigurationUpdate(
            expected_revision=state.revision,
            values=state.values,
            secrets={"token": SecretStr(token) if token else None},
        ),
    )
