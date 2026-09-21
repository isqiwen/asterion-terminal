"""Data-owned public projection for dependent functional workflows."""

from sqlalchemy import select

from asterion.data.public import SyncAccess, snapshot_backup_access
from asterion.platform.files import read_files
from asterion.platform.store import jobs


def access(storage, root):
    def inspect(transaction, identifiers, lock):
        with storage.borrow(transaction) as conn:
            query = select(jobs).where(jobs.c.id.in_(identifiers)).order_by(jobs.c.id)
            if lock:
                query = query.with_for_update()
            rows = list(conn.execute(query).mappings())
            if {r["id"] for r in rows} != set(identifiers):
                raise ValueError("同步依赖任务不存在")
            result = []
            for row in rows:
                payload = row["payload"]
                if row["kind"] != "data.sync" or payload["type_id"] != "futures.daily":
                    raise ValueError("续算依赖必须是日线同步任务")
                identity = payload["contract_identity"]
                if identity is None:
                    raise ValueError("同步依赖缺少固定合约身份")
                request = payload["request"]
                result.append(
                    {
                        "id": row["id"],
                        "state": row["state"],
                        "provider": request["provider"],
                        "connection_id": request.get("connection_id"),
                        "symbol": request["symbol"],
                        "exchange": request["exchange"],
                        "start": request["start"],
                        "end": request["end"],
                        "contracts_version_id": identity["catalog"]["inputs"][0]["version_id"],
                        "version_id": (row["result"] or {}).get("version_id"),
                    }
                )
            return result

    def versions(transaction):
        with storage.borrow(transaction) as conn:
            return snapshot_backup_access(conn, read_files(root))

    return SyncAccess(inspect, versions)
