"""Trusted built-in Python data-source plugins. A single catalogue owns their capabilities."""
from .akshare import Akshare
from .exchanges import Shfe, Ine, Czce
from .recorder import Recorder
from .tushare import TushareSource

PLUGINS = {plugin.identity: plugin for plugin in (TushareSource, Akshare, Recorder, Shfe, Ine, Czce)}


def plugin(identity):
    try:
        return PLUGINS[identity]
    except KeyError:
        raise ValueError("unknown data source") from None
