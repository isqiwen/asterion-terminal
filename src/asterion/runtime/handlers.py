"""Rebuild the distribution's approved task contributions in each spawned process."""

from asterion_bindings.plugin_host import PluginHost

from asterion.distribution import (  # noqa: F401 - worker assembly
    builtin_plugins,
    execution_resources,
)

handlers = PluginHost(builtin_plugins()).handlers
