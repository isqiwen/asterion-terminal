"""Read-only validation of data artifacts and encrypted source configurations."""

from collections.abc import Callable
from dataclasses import dataclass

import pyarrow as pa
import pyarrow.parquet as pq
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.catalog import ReferenceRelease, SourceIdentity
from asterion_bindings.files import ReadFiles
from asterion_bindings.recovery import BackupCheck
from sqlalchemy import select

from asterion.data.catalog import snapshots
from asterion.data.coverage import CoverageReport, reports
from asterion.data.history import HistoryPlan
from asterion.data.library import versions
from asterion.data.partitions import read_partition, verified_partitions
from asterion.data.preparation import PreparationJournal, batches
from asterion.data.reference_source import validate_catalog_input
from asterion.data.reference_store import releases
from asterion.platform.store import jobs


@dataclass(frozen=True)
class DataBackup:
    versions: tuple[dict, ...]
    references: tuple[dict, ...]
    coverage: tuple[dict, ...]
    preparations: tuple[dict, ...]
    snapshots: tuple[dict, ...]
    history_plans: tuple[dict, ...]
    # Recorded artifacts are verified through a read-only artifact grant; the
    # file grant only enumerates encrypted source configurations, which must
    # open with this runtime key.
    artifacts: ArtifactStore
    files: ReadFiles
    opens: Callable[[bytes], bool]


def load_evidence(conn, artifacts, files, opens):
    rows = tuple(
        dict(row)
        for row in conn.execute(
            select(versions.c.id, versions.c.manifest, versions.c.rows)
        ).mappings()
    )
    references = tuple(dict(row) for row in conn.execute(select(releases)).mappings())
    coverage = tuple(conn.execute(select(reports.c.report)).scalars())
    preparations = tuple(dict(row) for row in conn.execute(select(batches)).mappings())
    return DataBackup(
        rows,
        references,
        coverage,
        preparations,
        tuple(dict(r) for r in conn.execute(select(snapshots)).mappings()),
        tuple(
            p["history_plan"]
            for p in conn.execute(select(jobs.c.payload)).scalars()
            if "history_plan" in p
        ),
        artifacts,
        files,
        opens,
    )


def standard_rows(artifacts: ArtifactStore, manifest, partitions) -> list[dict]:
    if manifest["format"] == "partition_manifest":
        return [row for part in partitions for row in read_partition(artifacts, part)]
    content = artifacts.read(manifest["path"], manifest["checksum"], manifest["bytes"])
    return pq.read_table(pa.BufferReader(content)).to_pylist()


def validate_backup(evidence: DataBackup):
    row_counts = {row["id"]: row.get("rows") for row in evidence.versions}
    by_id = {row["id"]: row["manifest"] for row in evidence.versions}
    for value in evidence.history_plans:
        plan = HistoryPlan.model_validate(value)
        body = plan.request
        for identifier, type_id in (
            (body.contracts_version_id, "futures.contracts"),
            (body.calendar_version_id, "futures.calendar"),
        ):
            manifest = by_id.get(identifier)
            if (
                manifest is None
                or manifest["source"] != body.provider
                or manifest["layer"] != "STANDARD"
                or manifest["type"]["id"] != type_id
                or manifest["scope"].get("exchange") != body.exchange
                or manifest["scope"].get("connection_id") != body.connection_id
            ):
                raise ValueError("恢复后历史计划固定依据缺失或不匹配")
        for part in plan.slices:
            validate_catalog_input(
                part.identity.catalog.inputs[0], by_id[body.contracts_version_id]
            )
    for row in evidence.preparations:
        journal = PreparationJournal.model_validate(row)
        if journal.identity:
            for item in journal.identity.catalog.inputs:
                if item.version_id not in by_id:
                    raise ValueError("恢复后准备批次的固定资料缺失")
                validate_catalog_input(item, by_id[item.version_id])
    for row in evidence.references:
        release = ReferenceRelease.model_validate(row)
        for item in release.catalog.inputs:
            if item.version_id not in by_id:
                raise ValueError("恢复后目录输入版本缺失")
            validate_catalog_input(item, by_id[item.version_id])
    for row in evidence.coverage:
        report = CoverageReport.model_validate(row)
        for ident in (
            report.daily_version_id,
            report.calendar_version_id,
            report.contracts_version_id,
        ):
            if ident is not None and ident not in by_id:
                raise ValueError("恢复后覆盖报告输入缺失")
        if report.identity:
            for item in report.identity.catalog.inputs:
                validate_catalog_input(item, by_id[item.version_id])
    catalog = evidence.versions
    ids = {row["id"] for row in catalog}
    artifacts = evidence.artifacts
    for row in catalog:
        manifest = row["manifest"]
        try:
            artifacts.verify(manifest["path"], manifest["checksum"], manifest["bytes"])
        except (OSError, ValueError):
            raise ValueError("恢复后数据版本文件校验失败") from None
        if any(ident not in ids for ident in manifest.get("inputs", [])):
            raise ValueError("恢复后数据血缘引用缺失")
        partitions = ()
        if manifest["format"] == "partition_manifest":
            try:
                partitions = verified_partitions(artifacts, manifest)
            except (OSError, ValueError):
                raise ValueError("恢复后共享分区文件校验失败") from None
        if (
            manifest.get("source") != "local_file"
            and manifest.get("type", {}).get("id")
            in {"futures.daily", "futures.settlement", "futures.minute"}
            and not manifest.get("contract_identity")
        ):
            raise ValueError("合约数据缺少固定合约身份依据")
        if manifest.get("source") == "local_file":
            from asterion_bindings.catalog import ImportIdentity

            identity = ImportIdentity.model_validate(manifest["import_options"]["identity"])
            if manifest["layer"] == "STANDARD":
                identity.validate_rows(standard_rows(artifacts, manifest, partitions))
            for item in identity.catalog.inputs:
                if item.version_id not in by_id:
                    raise ValueError("恢复后导入身份目录输入缺失")
                validate_catalog_input(item, by_id[item.version_id])
        if manifest.get("contract_identity"):
            identity = SourceIdentity.model_validate(manifest["contract_identity"])
            for item in identity.catalog.inputs:
                if item.version_id not in by_id:
                    raise ValueError("恢复后同步身份输入缺失")
                validate_catalog_input(item, by_id[item.version_id])
            if manifest["layer"] == "STANDARD":
                rows = standard_rows(artifacts, manifest, partitions)
                if manifest["type"]["id"] == "futures.minute":
                    from asterion.data.minute import MinuteContext, bind_rows
                    from asterion.data.types import builtin_types

                    context = MinuteContext.model_validate(manifest.get("minute_context"))
                    if (
                        context.frequency != manifest["scope"].get("frequency")
                        or context.frequency != manifest["type"]["frequency"]
                    ):
                        raise ValueError("恢复后分钟周期与数据范围不一致")
                    if context.trading_time.id != manifest["scope"].get(
                        "trading_time_id"
                    ) or context.timestamp_semantics != manifest["scope"].get(
                        "timestamp_semantics"
                    ):
                        raise ValueError("恢复后分钟时间依据与数据范围不一致")
                    builtin_types().get("futures.minute").validate(rows)
                    for row in rows:
                        bind_rows([row], context, row["available_at"])
                if (
                    identity.validate_rows(rows, manifest["scope"]["exchange"])
                    != manifest["scope"]["contract_ids"]
                ):
                    raise ValueError("恢复后行情数据集范围与实际合约身份不一致")
    from asterion.data.public import Manifest

    for snapshot in evidence.snapshots:
        manifest = Manifest.model_validate(snapshot["manifest"])
        if manifest.storage == "parquet":
            try:
                artifacts.verify(f"published/{snapshot['id']}.parquet", manifest.checksum)
            except (OSError, ValueError):
                raise ValueError("恢复后图表文件校验失败") from None
        else:
            version = by_id.get(manifest.version_id)
            if (
                version is None
                or version["checksum"] != manifest.checksum
                or version["snapshot_id"] != snapshot["id"]
                or version["type"]["id"] != "futures.daily"
                or version["format"] != "partition_manifest"
                or row_counts.get(manifest.version_id) != manifest.rows
            ):
                raise ValueError("恢复后图表投影固定版本不一致")
    for name in evidence.files.scan(".credentials", ".enc"):
        if not evidence.opens(evidence.files.read(name)):
            raise ValueError("数据源配置快照无法用本机运行密钥读取")
    return {"versions": len(catalog), "reference_releases": len(evidence.references)}


check = BackupCheck(DataBackup, validate_backup)
