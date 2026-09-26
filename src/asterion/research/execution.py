"""Declared use of the L2 daily account capability."""

from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.resource import Resource

EXECUTION = Resource("execution.daily", ExecutionFactory)
