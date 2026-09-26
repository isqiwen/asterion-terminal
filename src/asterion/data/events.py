"""Data-owned facts; only immutable references cross the event channel."""

from asterion_bindings.events import Topic
from pydantic import BaseModel, ConfigDict


class VersionPublished(BaseModel):
    model_config = ConfigDict(extra="forbid")
    job_id: str
    dataset_id: str
    version_id: str
    checksum: str


VERSION_PUBLISHED = Topic(
    "data.version.published", "asterion.data", VersionPublished, "/data/catalog"
)
