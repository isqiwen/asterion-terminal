"""Built-in data functionality; all data APIs and state are owned here."""

from asterion.data.backup import check
from asterion.data.catalog_routes import catalog_router
from asterion.data.diagnostics import provider_services
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
from asterion.data.snapshots import Snapshots
from asterion.data.sync import DataSync
from asterion.data.sync_batch import submit_batch
from asterion.data.sync_dependencies import access as sync_access
from asterion.data.worker import task_handlers
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.plugins import Activation, Context, Plugin
from asterion.platform.resources import DATA_ROOT, STORAGE, TASKS


def activate(context: Context):
    from asterion.data.catalog import snapshots

    engine = context.resource(STORAGE)
    tasks = context.resource(TASKS)
    root = context.resource(DATA_ROOT)
    engine.initialize(snapshots)
    access = context.require(ACCOUNT_ACCESS)

    def published(transaction, job_id):
        for callback in context.hooks("data.sync_published"):
            callback(transaction, job_id)

    sync = DataSync(engine, tasks, root, context.resource(CREDENTIALS), published=published)
    data = Snapshots(engine, tasks, root)
    reference = ReferenceStore(engine)
    for directory in ("sources", "published", "artifacts", "backups"):
        (root / directory).mkdir(parents=True, exist_ok=True)

    def references(conn, version_id):
        counts = {"reference_catalogs": reference.references(conn, version_id)}
        for reader in context.hooks("data.references"):
            for key, count in reader(conn, version_id).items():
                if key in counts:
                    raise ValueError(f"Duplicate reference category: {key}")
                counts[key] = count
        return counts

    versions = VersionReader(engine, root)
    return Activation(
        close=context.resource(STORAGE).close,
        exports={
            VERSION_ACCESS: VersionAccess(versions.read, versions.coverage),
            SYNC_ACCESS: sync_access(engine, root),
            SYNC_BATCH_ACCESS: SyncBatchAccess(
                lambda transaction, body: submit_batch(sync, transaction, body)
            ),
        },
        routers=(
            router(sync, access.account, (references,)),
            catalog_router(data, reference, tasks, access.account, versions.read),
        ),
        hooks={"services": (lambda ready: provider_services(sync, ready),)},
    )


plugin = Plugin(
    "asterion.data",
    ("asterion.identity", "asterion.trading_time"),
    activate,
    handlers=task_handlers(),
    provides=(VERSION_ACCESS, SYNC_ACCESS, SYNC_BATCH_ACCESS),
    consumes=(ACCOUNT_ACCESS,),
    observes=("data.references", "data.sync_published"),
    resources=(STORAGE, DATA_ROOT, CREDENTIALS, TASKS),
    backup=check,
)
