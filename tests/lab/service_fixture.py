"""Replay tests select a prefilled version without requiring a network fixture."""
from asterion_lab.warehouse_client import Dataset
from asterion_lab.service import Service as Lab
from warehouse_writer import Dataset as Written


class Service(Lab):
    def dataset(self):
        directory = self.directory
        class Cached(Dataset):
            def current(self):
                return self.version(Written(directory).current().identity)
        return Cached(directory)
