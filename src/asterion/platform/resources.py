from asterion_bindings.resource import Resource

"""Bootstrap resource contracts; distribution assembly assigns individual grants."""

from pathlib import Path

from asterion_bindings.storage import Storage
from asterion_bindings.task_repository import TaskPort

STORAGE = Resource("runtime.storage", Storage)
DATA_ROOT = Resource("runtime.data_root", Path)
TASKS = Resource("runtime.tasks", TaskPort)
