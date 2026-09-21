from asterion.platform.resource import Resource

"""Bootstrap resource contracts; distribution assembly assigns individual grants."""

from pathlib import Path

from asterion.platform.storage import Storage
from asterion.platform.task_port import TaskPort

STORAGE = Resource("runtime.storage", Storage)
DATA_ROOT = Resource("runtime.data_root", Path)
TASKS = Resource("runtime.tasks", TaskPort)
