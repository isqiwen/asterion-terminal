from signal_logic import Momentum, warmup

from asterion_plugin_sdk import serve_strategy

serve_strategy(Momentum, warmup)
