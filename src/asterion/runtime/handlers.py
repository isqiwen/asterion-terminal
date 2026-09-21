"""Rebuild the distribution's approved task contributions in each spawned process."""

from asterion.distribution import (  # noqa: F401 - worker assembly
    builtin_plugins,
    execution_resources,
)
from asterion.platform.plugins import PluginHost

handlers = PluginHost(builtin_plugins()).handlers
