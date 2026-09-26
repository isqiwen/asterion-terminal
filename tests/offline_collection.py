"""Offline collection for acceptance tool tests: the tools' Tushare collection
replaced by recorded vendor responses, never the network."""

from datetime import UTC, datetime

from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.platform.serialization import canonical


def offline(fetch):
    """A `collect_evidence` that plans with Tushare and records each partition."""

    def collect_evidence(sync, credentials, job) -> bytes:
        provider = Tushare()
        request = SyncRequest.model_validate(job["payload"]["request"])
        evidence = []
        for index, partition in enumerate(provider.plan(request)):
            rows = fetch(provider, partition, {})
            value = {
                "partition": partition.model_dump(),
                "rows": [{f: row.get(f) for f in partition.fields} for row in rows],
                "observed_at": datetime.now(UTC).isoformat(),
            }
            sync.evidence.record(job["id"], job["token"], index, value)
            evidence.append(value)
        return canonical(evidence)

    return collect_evidence
