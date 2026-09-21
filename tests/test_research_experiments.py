from dataclasses import replace

import pytest
from sqlalchemy import select
from storage_support import scheduler
from test_research import services  # noqa: F401

from asterion.platform.serialization import canonical
from asterion.platform.store import jobs
from asterion.platform.tasks.service import Conflict
from asterion.research.engine import calculate
from asterion.research.experiments import ExperimentRequest, Experiments


def setup(request):
    service, base, _ = request.getfixturevalue("services")
    base = base.model_copy(update={"parameters": {"fast": 1, "slow": 3}})
    return (
        service,
        Experiments(service),
        ExperimentRequest(name="均线候选", base=base, grid={"fast": [1, 2], "slow": [3, 4]}),
    )


def test_atomic_submission_idempotence_and_owner_isolation(request):
    service, experiments, body = setup(request)
    result = experiments.submit("alice", body)
    assert len(result["items"]) == 4
    assert experiments.submit("alice", body)["runs"] == result["runs"]
    assert len(service.list()) == 4
    assert experiments.list("bob") == []
    with pytest.raises(KeyError):
        experiments.get("bob", result["id"])
    with pytest.raises(Conflict):
        experiments.submit("alice", body.model_copy(update={"name": "changed"}))
    with service.engine.connect() as conn:
        payloads = conn.execute(select(jobs.c.payload)).scalars().all()
    assert all(
        p["bars"] == payloads[0]["bars"]
        and p["request"]["strategy"] == payloads[0]["request"]["strategy"]
        for p in payloads
    )
    assert len({p["input_checksum"] for p in payloads}) == 4


@pytest.mark.parametrize(
    "grid",
    [
        {"fast": [1, 3]},
        {"fast": [1, True]},
        {"fast": [1, 1]},
        {"missing": [1, 2]},
        {"fast": [1]},
        {"slow": [3, 50]},
        {"fast": list(range(1, 34))},
    ],
)
def test_invalid_combinations_leave_no_records(request, grid):
    service, experiments, body = setup(request)
    with pytest.raises(ValueError):
        experiments.submit("alice", body.model_copy(update={"grid": grid}))
    assert experiments.list("alice") == [] and service.list() == []


def test_mid_transaction_failure_rolls_back_jobs_and_experiment(request):
    service, experiments, body = setup(request)
    submit_batch = service.tasks.submit_batch

    def fail(conn, commands):
        submit_batch(conn, commands)
        raise RuntimeError("injected failure")

    service.tasks = replace(service.tasks, submit_batch=fail)
    with pytest.raises(RuntimeError):
        experiments.submit("alice", body)
    assert service.list() == [] and experiments.list("alice") == []


def test_cancel_preserves_published_result_and_revokes_other_leases(request):
    service, experiments, body = setup(request)
    record = experiments.submit("alice", body)
    tasks = scheduler(service.engine)
    first = tasks.claim("first")
    service.publish(
        first["id"], first["token"], canonical(calculate(first["payload"], service.strategies))
    )
    running = tasks.claim("second")
    with pytest.raises(KeyError):
        experiments.cancel("bob", record["id"])
    cancelled = experiments.cancel("alice", record["id"])
    assert [i["state"] for i in cancelled["items"]].count("SUCCEEDED") == 1
    assert [i["state"] for i in cancelled["items"]].count("CANCELLED") == 3
    assert (
        next(i for i in cancelled["items"] if i["id"] == first["id"])["result"]["net_profit"]
        is not None
    )
    assert experiments.cancel("alice", record["id"])["runs"] == record["runs"]
    with pytest.raises(Conflict):
        service.publish(
            running["id"],
            running["token"],
            canonical(calculate(running["payload"], service.strategies)),
        )


def test_task_port_rejects_cancelling_foreign_kind(request):
    service, _, _ = setup(request)
    tasks = scheduler(service.engine)
    foreign = tasks.submit("foreign", "data.sync", {})
    with service.engine.begin() as conn, pytest.raises(ValueError, match="not granted"):
        service.tasks.cancel_batch(conn, [foreign["id"]])
    assert tasks.get(foreign["id"])["state"] == "QUEUED"


def test_concurrent_retries_create_one_experiment(request):
    from concurrent.futures import ThreadPoolExecutor

    service, experiments, body = setup(request)
    with ThreadPoolExecutor(max_workers=2) as pool:
        records = list(pool.map(lambda _: experiments.submit("alice", body), range(2)))
    assert records[0]["runs"] == records[1]["runs"]
    assert len(service.list()) == 4 and len(experiments.list("alice")) == 1


def test_backup_rejects_broken_experiment_association(request):
    from asterion.research.backup import ResearchBackup, validate_backup

    _, experiments, body = setup(request)
    record = experiments.submit("alice", body)
    empty = lambda: iter(())
    evidence = ResearchBackup(
        experiments.service.strategies,
        lambda _: None,
        empty,
        empty,
        empty,
        lambda: iter([(record["spec"], record["runs"], record["checksum"], set(record["runs"]))]),
        empty,
    )
    validate_backup(evidence)
    with pytest.raises(ValueError, match="实验"):
        validate_backup(
            replace(
                evidence,
                experiments=lambda: iter(
                    [(record["spec"], record["runs"], record["checksum"], set())]
                ),
            )
        )
