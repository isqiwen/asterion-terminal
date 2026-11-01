"""Replay tests select a prefilled version without requiring a network fixture."""
from asterion_lab.market import Dataset
from asterion_lab.service import Service as Lab
from warehouse_writer import dataset_of


class Service(Lab):
    def dataset_for(self, provider, origin=None):
        directory = self.directory
        class Cached(Dataset):
            def current(self):
                return self.version(dataset_of(directory, provider).current().identity)
        return Cached(directory, provider)
