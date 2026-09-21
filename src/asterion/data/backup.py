"""Read-only validation of data artifacts and encrypted source configurations."""

import io
import json
from collections.abc import Callable
from dataclasses import dataclass

import pyarrow.parquet as pq
from sqlalchemy import select

from asterion.data.coverage import CoverageReport, reports
from asterion.data.library import versions
from asterion.data.preparation import PreparationJournal, batches
from asterion.data.reference import SourceIdentity
from asterion.data.reference_source import validate_catalog_input
from asterion.data.reference_store import ReferenceRelease, releases
from asterion.platform.backup import BackupCheck
from asterion.platform.files import ReadFiles


@dataclass(frozen=True)
class DataBackup:
    versions: tuple[dict, ...]
    references: tuple[dict, ...]
    coverage: tuple[dict, ...]
    preparations: tuple[dict, ...]
    files: ReadFiles
    decrypt: Callable[[bytes], bytes]


def load_evidence(conn, files, decrypt):
    rows = tuple(
        dict(row) for row in conn.execute(select(versions.c.id, versions.c.manifest)).mappings()
    )
    references = tuple(dict(row) for row in conn.execute(select(releases)).mappings())
    coverage = tuple(conn.execute(select(reports.c.report)).scalars())
    preparations = tuple(dict(row) for row in conn.execute(select(batches)).mappings())
    return DataBackup(rows, references, coverage, preparations, files, decrypt)


def validate_backup(evidence: DataBackup):
    by_id = {row["id"]: row["manifest"] for row in evidence.versions}
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
    for row in catalog:
        manifest = row["manifest"]
        if (
            manifest.get("source") != "local_file"
            and manifest.get("type", {}).get("id") in {"futures.daily", "futures.settlement"}
            and not manifest.get("contract_identity")
        ):
            raise ValueError("合约数据缺少固定合约身份依据")
        if manifest.get("source") == "local_file":
            from asterion.data.public import ImportOptions

            identity = ImportOptions.model_validate(manifest["import_options"]).identity
            if manifest["layer"] == "STANDARD":
                rows = pq.read_table(io.BytesIO(evidence.files.read(manifest["path"]))).to_pylist()
                identity.validate_rows(rows)
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
                if manifest["format"] == "partition_manifest":
                    parts = json.loads(evidence.files.read(manifest["path"]))["partitions"]
                    paths = ["artifacts/" + part["checksum"] + ".parquet" for part in parts]
                else:
                    paths = [manifest["path"]]
                rows = [
                    row
                    for path in paths
                    for row in pq.read_table(io.BytesIO(evidence.files.read(path))).to_pylist()
                ]
                if (
                    identity.validate_rows(rows, manifest["scope"]["exchange"])
                    != manifest["scope"]["contract_ids"]
                ):
                    raise ValueError("恢复后行情数据集范围与实际合约身份不一致")
        name = manifest["path"]
        if evidence.files.digest(name) != manifest["checksum"]:
            raise ValueError("恢复后数据版本文件校验失败")
        if any(ident not in ids for ident in manifest.get("inputs", [])):
            raise ValueError("恢复后数据血缘引用缺失")
        if manifest["format"] == "partition_manifest":
            for part in json.loads(evidence.files.read(name))["partitions"]:
                digest = part["checksum"]
                if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                    raise ValueError("分区校验和格式错误")
                if evidence.files.digest("artifacts/" + digest + ".parquet") != digest:
                    raise ValueError("恢复后共享分区文件校验失败")
    for name in evidence.files.scan(".credentials", ".enc"):
        evidence.decrypt(evidence.files.read(name))
    return {"versions": len(catalog), "reference_releases": len(evidence.references)}


check = BackupCheck(DataBackup, validate_backup)
