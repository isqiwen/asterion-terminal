from uuid import UUID

from asterion_bindings.task_models import Job
from fastapi import APIRouter, Depends, Header, HTTPException, Request
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from asterion.research.engine import BacktestRequest
from asterion.research.experiments import ExperimentRequest, Experiments
from asterion.research.packages import LIMIT_BYTES, ResearchPackages, parse_package
from asterion.research.strategies import StrategyInfo
from asterion.research.validation import Selection, Validations
from asterion.research.workspace import DeleteDocument, DocumentUpdate, ResearchWorkspace


class Rerun(BaseModel):
    command_id: str = Field(min_length=1, max_length=100)


def router(service, account_access, owner_access):
    routes = APIRouter(prefix="/api/v1")
    access = [Depends(account_access)]
    workspace = ResearchWorkspace(service.engine)
    bundles = ResearchPackages(service)
    experiments = Experiments(service)
    validations = Validations(service)

    @routes.post("/research/experiments", dependencies=access, status_code=202)
    def submit_experiment(body: ExperimentRequest, owner: str = Depends(owner_access)):
        return experiments.submit(owner, body)

    @routes.get("/research/experiments", dependencies=access)
    def list_experiments(owner: str = Depends(owner_access)):
        return {"items": experiments.list(owner)}

    @routes.get("/research/experiments/{identifier}", dependencies=access)
    def get_experiment(identifier: str, owner: str = Depends(owner_access)):
        try:
            return experiments.get(owner, identifier)
        except KeyError:
            raise HTTPException(404, "当前账户没有此实验") from None

    @routes.post("/research/experiments/{identifier}/cancel", dependencies=access)
    def cancel_experiment(identifier: str, owner: str = Depends(owner_access)):
        try:
            return experiments.cancel(owner, identifier)
        except KeyError:
            raise HTTPException(404, "当前账户没有此实验") from None

    @routes.get("/research/experiments/{identifier}/validation", dependencies=access)
    def validation(identifier: str, owner: str = Depends(owner_access)):
        try:
            return validations.get(owner, identifier)
        except KeyError:
            raise HTTPException(404, "当前账户没有此实验") from None

    @routes.post(
        "/research/experiments/{identifier}/validation", dependencies=access, status_code=202
    )
    def validate_selection(identifier: str, body: Selection, owner: str = Depends(owner_access)):
        try:
            return validations.submit(owner, identifier, body)
        except KeyError:
            raise HTTPException(404, "当前账户没有此实验或研究运行") from None

    @routes.post("/research/experiments/{identifier}/validation/cancel", dependencies=access)
    def cancel_validation(identifier: str, owner: str = Depends(owner_access)):
        try:
            return validations.cancel(owner, identifier)
        except KeyError:
            raise HTTPException(404, "当前账户没有此验证") from None

    @routes.get("/research/experiments/{identifier}/validation/export", dependencies=access)
    def export_validation(identifier: str, owner: str = Depends(owner_access)):
        import base64
        import io
        import zipfile

        from asterion.platform.serialization import canonical

        try:
            record = validations.get(owner, identifier)
        except KeyError:
            raise HTTPException(404, "当前账户没有此实验") from None
        if not record or record["validation"]["state"] != "SUCCEEDED":
            raise ValueError("验证成功后才能导出完整复现资料")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(
                "selection.json", canonical({k: v for k, v in record.items() if k != "validation"})
            )
            archive.writestr(
                "research.json", canonical(bundles.export(record["selection"]["source_run"], True))
            )
            archive.writestr("validation.json", canonical(bundles.export(record["run_id"], True)))
            archive.writestr(
                "README.txt",
                "selection.json 保存冻结选择及边界。将 research.json 和 validation.json 分别导入研究复现入口核验和重放。ZIP 不包含可执行插件；需要显式安装启用相同工件。",
            )
        return {
            "filename": f"validation-{identifier[:12]}.zip",
            "content_base64": base64.b64encode(buffer.getvalue()).decode(),
        }

    @routes.get("/research/strategies", response_model=list[StrategyInfo], dependencies=access)
    def strategies():
        return service.strategies.list()

    @routes.get("/research/runs/{run_id}/export", dependencies=access)
    def export_run(run_id: str, include_data: bool = False):
        try:
            return bundles.export(run_id, include_data)
        except KeyError:
            raise HTTPException(404, "研究运行不存在") from None

    @routes.get("/research/runs/{run_id}/results-archive", dependencies=access)
    def export_results(run_id: str):
        try:
            return bundles.result_archive(run_id)
        except KeyError:
            raise HTTPException(404, "研究运行不存在") from None

    @routes.post("/research/packages", dependencies=access)
    async def import_package(request: Request, owner: str = Depends(owner_access)):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > LIMIT_BYTES:
                raise HTTPException(413, "复现包超过 8 MB")
        try:
            return await run_in_threadpool(bundles.receive, owner, parse_package(bytes(content)))
        except (RecursionError, TypeError):
            raise ValueError("复现包结构超出支持边界") from None

    @routes.post("/research/packages/{package_id}/check", dependencies=access)
    def inspect_package(package_id: str, owner: str = Depends(owner_access)):
        try:
            return bundles.inspect(owner, package_id)
        except KeyError:
            raise HTTPException(404, "当前账户没有此复现包") from None

    @routes.post(
        "/research/packages/{package_id}/replay",
        response_model=Job,
        status_code=202,
        dependencies=access,
    )
    def replay_package(package_id: str, body: Rerun, owner: str = Depends(owner_access)):
        try:
            return bundles.replay(owner, package_id, body.command_id)
        except KeyError:
            raise HTTPException(404, "当前账户没有此复现包或所需数据") from None

    @routes.get("/research/workspace", dependencies=access)
    def documents(owner: str = Depends(owner_access)):
        return workspace.read(owner)

    @routes.post("/research/workspace/draft", dependencies=access)
    def save_draft(body: DocumentUpdate, owner: str = Depends(owner_access)):
        return workspace.save(owner, "draft", body)

    @routes.post("/research/templates/{template_id}", dependencies=access)
    def save_template(template_id: UUID, body: DocumentUpdate, owner: str = Depends(owner_access)):
        return workspace.save(owner, str(template_id), body)

    @routes.post("/research/templates/{template_id}/delete", dependencies=access)
    def delete_template(
        template_id: UUID, body: DeleteDocument, owner: str = Depends(owner_access)
    ):
        return workspace.delete(owner, str(template_id), body.expected_revision)

    @routes.post("/research/runs", response_model=Job, status_code=202, dependencies=access)
    def submit(body: BacktestRequest):
        return service.submit(body)

    @routes.get("/research/runs", dependencies=access)
    def listing():
        return service.list()

    @routes.get("/research/runs/{run_id}", dependencies=access)
    def detail(run_id: str):
        try:
            return service.get(run_id)
        except KeyError:
            raise HTTPException(404, "研究运行不存在") from None

    @routes.post(
        "/research/runs/{run_id}/rerun", response_model=Job, status_code=202, dependencies=access
    )
    def rerun(run_id: str, body: Rerun):
        try:
            return service.rerun(run_id, body.command_id)
        except KeyError:
            raise HTTPException(404, "研究运行不存在") from None

    @routes.post("/jobs/{job_id}/publish-research")
    async def publish(job_id: str, request: Request, x_lease_token: str = Header()):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > 8_000_000:
                raise HTTPException(413, "研究结果超过 8 MB")
        return await run_in_threadpool(service.publish, job_id, x_lease_token, bytes(content))

    return routes
