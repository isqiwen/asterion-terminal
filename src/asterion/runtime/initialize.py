"""Initialize the selected distribution without starting workers or serving HTTP."""

from asterion.api.app import create_app


def initialize(settings, engine):
    app = create_app(settings, engine)
    app.state.plugins.close()
