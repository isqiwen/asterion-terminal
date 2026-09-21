"""Atomic daily sync admission shared by functional workflow consumers."""

from asterion.data.public import DailySyncBatch
from asterion.data.sync_admission import SyncSubmission, prepare
from asterion.platform.tasks.public import Job


def submit_batch(sync, transaction, body: DailySyncBatch):
    provider = sync.registry.get(body.provider)
    capabilities = [c for c in provider.manifest.capabilities if c.type_id == "futures.daily"]
    if len(capabilities) != 1:
        raise ValueError("数据源必须声明唯一日线采集能力")
    commands = []
    with sync.engine.join(transaction) as conn:
        for index, symbol in enumerate(sorted(body.symbols)):
            request, identity = prepare(
                sync,
                SyncSubmission(
                    command_id=f"{body.command_prefix}:{index}",
                    provider=body.provider,
                    connection_id=body.connection_id,
                    exchange=body.exchange,
                    dataset=capabilities[0].id,
                    symbol=symbol,
                    start=body.trading_day,
                    end=body.trading_day,
                    contracts_version_id=body.contracts_version_id,
                ),
            )
            payload = sync.submission_payload(request) | {"contract_identity": identity}
            sync.validate_identity(conn, payload)
            commands.append((request.command_id, "data.sync", payload))
        # All source plans, credentials and identities must pass before any job is inserted.
        return tuple(Job.model_validate(row) for row in sync.tasks.submit_batch(conn, commands))
