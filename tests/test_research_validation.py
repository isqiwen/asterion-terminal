from datetime import date

import pytest
from import_identity_support import import_identity
from storage_support import data_store, domain_tasks, scheduler
from test_research import services  # noqa: F401

from asterion.data.importing import encode_import
from asterion.data.library import DataLibrary
from asterion.data.public import ImportOptions, ImportRequest
from asterion.data.snapshots import Snapshots
from asterion.platform.serialization import canonical
from asterion.platform.tasks.service import Conflict
from asterion.research.engine import calculate
from asterion.research.experiments import ExperimentRequest, Experiments
from asterion.research.validation import Selection, Validations, validate_record


def setup(request):
    service, base, root = request.getfixturevalue("services")
    tasks = scheduler(service.engine)
    csv = "contract,trading_day,open,high,low,close,vol,settle\n" + "\n".join(
        f"SHFE.rb2405,2024-01-{i:02},{i + 10},{i + 11},{i + 10},{i + 11},100,{i + 11}"
        for i in range(1, 11)
    )
    body = ImportRequest(
        command_id="validation-data",
        source="fixture",
        csv=csv,
        options=ImportOptions(
            identity=import_identity("SHFE.rb2405"),
            type_id="futures.daily",
            frequency="1d",
            source_id="validation",
        ),
    )
    tasks.submit(
        body.command_id, "data.import_csv", body.model_dump(exclude={"command_id"}, mode="json")
    )
    job = tasks.claim("import-validation")
    Snapshots(
        data_store(service.engine), domain_tasks(service.engine, "data"), root / "data"
    ).publish(job["id"], job["token"], encode_import(job["payload"])[0])
    version = next(
        v
        for v in DataLibrary(data_store(service.engine), root / "data").list(
            type_id="futures.daily", layer="STANDARD"
        )["items"]
        if v["rows"] == 10
    )
    from reference_support import research_coverage

    training = research_coverage(
        data_store(service.engine), root / "data", version["id"], "2024-01-01", "2024-01-04"
    )
    validation = research_coverage(
        data_store(service.engine), root / "data", version["id"], "2024-01-05", "2024-01-10"
    )
    base = base.model_copy(
        update={
            "version_id": version["id"],
            "coverage_report_id": training["id"],
            "start": date(2024, 1, 1),
            "end": date(2024, 1, 4),
            "parameters": {"fast": 1, "slow": 2},
        }
    )
    experiment = Experiments(service).submit(
        "alice", ExperimentRequest(name="training", base=base, grid={"slow": [2, 3]})
    )
    for _ in range(2):
        job = tasks.claim("research")
        service.publish(
            job["id"], job["token"], canonical(calculate(job["payload"], service.strategies))
        )
    selected = Selection(
        source_run=experiment["runs"][0],
        start=date(2024, 1, 5),
        end=date(2024, 1, 10),
        coverage_report_id=validation["id"],
        coverage_policy="allow_incomplete",
        coverage_note="fixture coverage",
        reason="预先选择较短预热",
    )
    return service, experiment, Validations(service), selected


def test_frozen_selection_independent_warmup_and_result(request):
    service, experiment, validations, selected = setup(request)
    record = validations.submit("alice", experiment["id"], selected)
    assert record["evidence"]["warmup_start"] == "2024-01-05"
    assert record["evidence"]["warmup_end"] == "2024-01-06"
    assert record["evidence"]["evaluation_start"] == "2024-01-07"
    assert validations.submit("alice", experiment["id"], selected)["run_id"] == record["run_id"]
    with pytest.raises(Conflict):
        validations.submit(
            "alice",
            experiment["id"],
            selected.model_copy(update={"source_run": experiment["runs"][1]}),
        )
    claimed = scheduler(service.engine).claim("validation")
    output = calculate(claimed["payload"], service.strategies)
    assert [p["position"] for p in output["curve"][:2]] == [0, 0]
    assert output["fills"][0]["day"] == "2024-01-07"
    assert output["summary"]["initial_equity"] == "1000"
    assert (
        claimed["payload"]["request"]["parameters"]
        == record["evidence"]["research"]["request"]["parameters"]
    )
    service.publish(claimed["id"], claimed["token"], canonical(output))
    assert validations.get("alice", experiment["id"])["validation"]["state"] == "SUCCEEDED"
    validate_record(record, set(experiment["runs"] + [record["run_id"]]), {experiment["id"]})
    with pytest.raises(ValueError):
        validate_record(record, set(experiment["runs"]), {experiment["id"]})


@pytest.mark.parametrize(
    "change",
    [
        {"start": date(2024, 1, 4)},
        {"end": date(2024, 1, 6)},
        {"source_run": "foreign"},
        {"reason": " "},
    ],
)
def test_invalid_selection_creates_no_validation(request, change):
    service, experiment, validations, selected = setup(request)
    with pytest.raises(ValueError):
        validations.submit("alice", experiment["id"], selected.model_copy(update=change))
    assert validations.get("alice", experiment["id"]) is None
    assert len(service.list()) == 2


def test_owner_and_cancel_preserve_choice(request):
    service, experiment, validations, selected = setup(request)
    with pytest.raises(KeyError):
        validations.submit("bob", experiment["id"], selected)
    record = validations.submit("alice", experiment["id"], selected)
    assert validations.cancel("alice", experiment["id"])["validation"]["state"] == "CANCELLED"
    assert validations.submit("alice", experiment["id"], selected)["run_id"] == record["run_id"]
    assert len(service.list()) == 3


def test_concurrent_selection_and_atomic_rollback(request):
    from concurrent.futures import ThreadPoolExecutor
    from dataclasses import replace

    service, experiment, validations, selected = setup(request)
    original = service.tasks.submit_batch

    def fail(conn, commands):
        original(conn, commands)
        raise RuntimeError("injected interruption")

    service.tasks = replace(service.tasks, submit_batch=fail)
    with pytest.raises(RuntimeError):
        validations.submit("alice", experiment["id"], selected)
    assert validations.get("alice", experiment["id"]) is None
    assert len(service.list()) == 2
    service.tasks = replace(service.tasks, submit_batch=original)
    with ThreadPoolExecutor(max_workers=2) as pool:
        records = list(
            pool.map(lambda _: validations.submit("alice", experiment["id"], selected), range(2))
        )
    assert records[0]["run_id"] == records[1]["run_id"]
    assert len(service.list()) == 3
