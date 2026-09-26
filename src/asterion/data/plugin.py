"""Built-in data functionality; all data APIs and state are owned here."""

from asterion_bindings.plugin_host import Activation, Context, Plugin

from asterion.data.backup import check
from asterion.data.catalog_routes import catalog_router
from asterion.data.diagnostics import provider_services
from asterion.data.events import VERSION_PUBLISHED
from asterion.data.public import (
    CREDENTIALS,
    SYNC_ACCESS,
    SYNC_BATCH_ACCESS,
    VERSION_ACCESS,
    SyncBatchAccess,
    VersionAccess,
    VersionReader,
)
from asterion.data.reference_store import ReferenceStore
from asterion.data.routes import router
from asterion.data.sync import DataSync
from asterion.data.sync_batch import submit_batch
from asterion.data.sync_dependencies import access as sync_access
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.resources import DATA_ROOT, STORAGE, TASKS


def activate(context: Context):
    from asterion.data.catalog import snapshots

    engine = context.resource(STORAGE)
    tasks = context.resource(TASKS)
    root = context.resource(DATA_ROOT)
    engine.initialize(snapshots)
    access = context.require(ACCOUNT_ACCESS)

    def published(transaction, job_id):
        from sqlalchemy import select

        from asterion.data.library import versions

        records = transaction.execute(
            select(versions).where(versions.c.job_id == job_id)
        ).mappings()
        for record in records:
            if record["manifest"]["layer"] == "STANDARD":
                context.publish(
                    transaction,
                    VERSION_PUBLISHED,
                    record["dataset_id"],
                    {
                        "job_id": job_id,
                        "dataset_id": record["dataset_id"],
                        "version_id": record["id"],
                        "checksum": record["manifest"]["checksum"],
                    },
                )
        for callback in context.hooks("data.sync_published"):
            callback(transaction, job_id)

    sync = DataSync(engine, tasks, root, context.resource(CREDENTIALS), published=published)
    reference = ReferenceStore(engine)

    versions = VersionReader(engine, root)
    return Activation(
        close=context.resource(STORAGE).close,
        exports={
            VERSION_ACCESS: VersionAccess(versions.read, versions.coverage, versions.scan),
            SYNC_ACCESS: sync_access(engine, root),
            SYNC_BATCH_ACCESS: SyncBatchAccess(
                lambda transaction, body: submit_batch(sync, transaction, body)
            ),
        },
        routers=(
            router(sync, access.account),
            catalog_router(reference, access.account, versions.read),
        ),
        hooks={"services": (lambda ready: provider_services(sync, ready),)},
    )


plugin = Plugin(
    "asterion.data",
    ("asterion.identity",),
    activate,
    provides=(VERSION_ACCESS, SYNC_ACCESS, SYNC_BATCH_ACCESS),
    consumes=(ACCOUNT_ACCESS,),
    observes=("data.sync_published",),
    resources=(STORAGE, DATA_ROOT, CREDENTIALS, TASKS),
    backup=check,
    publishes=(VERSION_PUBLISHED,),
)
