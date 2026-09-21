"""Product-facing package management; installer and process mechanisms live in platform."""

import base64
import binascii
import hashlib
import json

from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field

from asterion.extensions.public import PACKAGES
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.plugins import Activation, Context, Plugin


class Selection(BaseModel):
    model_config = ConfigDict(extra="forbid")
    digest: str = Field(pattern=r"^[0-9a-f]{64}$")
    enabled: bool
    trust_local_code: bool


class Removal(BaseModel):
    model_config = ConfigDict(extra="forbid")
    digest: str = Field(pattern=r"^[0-9a-f]{64}$")


def activate(context: Context):
    packages = context.resource(PACKAGES)
    access = context.require(ACCOUNT_ACCESS)
    routes = APIRouter(prefix="/api/v1/extensions", dependencies=[Depends(access.account)])

    @routes.get("")
    def installed():
        return {
            "items": packages.list(),
            "trust": "local-code",
            "description": "本地插件以当前用户权限运行，仅启用你信任的代码。",
        }

    @routes.get("/{identifier}/diagnostics")
    def diagnostics(identifier: str):
        from asterion.platform.extensions.diagnostics import recent

        record = next(
            (item for item in packages.list() if item["manifest"]["id"] == identifier), None
        )
        if record is None:
            raise HTTPException(404, "插件未安装")
        return {"digest": record["digest"], "items": recent(packages.root, record["digest"])}

    async def uploaded(request: Request, expected: set[str]):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > 22_000_000:
                raise HTTPException(413, "插件包超过 16 MB")
        try:
            value = json.loads(content)
            if not isinstance(value, dict) or set(value) != expected:
                raise ValueError()
            archive = base64.b64decode(value["archive"], validate=True)
        except (ValueError, TypeError, binascii.Error):
            raise HTTPException(422, "插件包上传格式无效") from None
        return value, archive

    @routes.post("/inspect")
    async def inspect(request: Request):
        from starlette.concurrency import run_in_threadpool

        _, archive = await uploaded(request, {"archive"})
        return await run_in_threadpool(packages.inspect, archive)

    @routes.post("/install")
    async def install(request: Request):
        from starlette.concurrency import run_in_threadpool

        value, archive = await uploaded(request, {"archive", "digest", "trust_local_code"})
        if value["trust_local_code"] is not True:
            raise HTTPException(422, "安装并启用前需要确认信任该插件的本地代码")
        if value["digest"] != hashlib.sha256(archive).hexdigest():
            raise HTTPException(409, "插件包与确认内容不一致，请重新选择文件")
        return await run_in_threadpool(packages.install, archive, enabled=True)

    @routes.post("/{identifier}/state")
    def state(identifier: str, body: Selection):
        if body.enabled and not body.trust_local_code:
            raise HTTPException(422, "启用前需要确认信任该工件的本地代码")
        return packages.select(identifier, body.digest, body.enabled)

    @routes.post("/{identifier}/remove")
    def remove(identifier: str, body: Removal):
        return packages.remove(identifier, body.digest)

    @routes.get("/views")
    def views():
        return {
            "items": [
                {
                    "plugin_id": record["manifest"]["id"],
                    "digest": record["digest"],
                    "view": record["manifest"]["contributions"]["ui.table"],
                }
                for record in packages.list()
                if record["enabled"] and "ui.table" in record["manifest"]["contributions"]
            ]
        }

    @routes.post("/{identifier}/view")
    def view(identifier: str, body: Removal):
        from asterion.platform.extensions.process import call_package
        from asterion.platform.extensions.views import TableView, validate_rows

        record = next(
            (item for item in packages.list() if item["manifest"]["id"] == identifier), None
        )
        if not record or not record["enabled"] or record["digest"] != body.digest:
            raise HTTPException(409, "插件已停用、移除或工件已变化，请刷新视图")
        manifest, path = packages.resolve(body.digest)
        if "ui.table" not in manifest.contributions:
            raise HTTPException(404, "该插件未声明表格视图")
        definition = TableView.model_validate(manifest.contributions["ui.table"])
        rows = call_package(
            path,
            "view",
            {"id": definition.id},
            authorized=lambda: packages.enabled(identifier, body.digest),
        )
        return {"rows": validate_rows(definition, rows)}

    return Activation(routers=(routes,))


plugin = Plugin(
    "asterion.extensions",
    ("asterion.identity",),
    activate,
    resources=(PACKAGES,),
    consumes=(ACCOUNT_ACCESS,),
)
