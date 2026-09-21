"""Offline consecutive publication evidence, not historical market-time attestation."""

import pytest
from role_source_support import port
from test_computed_roles import evidence

from asterion.contract_roles.computed import ComputedSources
from asterion.contract_roles.computed_public import ComputedRequest, ComputedVersion, digest
from asterion.contract_roles.sequence import (
    ContinuationRequest,
    SequenceRequest,
    check_extension,
    continuation,
    verify_sequence,
)


def campaign():
    values, request = evidence()
    service = ComputedSources(port(values))
    payload = request.model_dump()
    payload["daily_inputs"] = request.daily_inputs[:2]
    first_spec = service.build(ComputedRequest.model_validate(payload))
    first = publish(first_spec, "2025-04-10T16:00:00+08:00")
    body = ContinuationRequest(
        previous_version_id=first.id,
        daily_inputs=request.daily_inputs[2:],
        explanation="offline next trading day",
    )
    second_spec = continuation(service, first, body)
    second = publish(second_spec, "2025-04-11T16:00:00+08:00")
    return values, service, first, second, body


def publish(spec, when):
    return ComputedVersion(id=digest(spec.model_dump(mode="json")), spec=spec, published_at=when)


def check(service, *records, minimum=1):
    by_id = {r.id: r for r in records}
    return verify_sequence(
        service,
        by_id.__getitem__,
        SequenceRequest(version_ids=tuple(by_id), minimum_switches=minimum),
    )


def test_continuation_preserves_confirmation_and_verifies_actual_switch():
    _, service, first, second, _ = campaign()
    assert first.spec.result.decisions[-1].reason == "confirming"
    assert second.spec.result.decisions[-1].reason == "switched"
    result = check(service, first, second)
    assert result.switches == 1
    assert result.observation_days == ("2025-04-10", "2025-04-11")
    assert not result.execution_authorized


@pytest.mark.parametrize("change", ["late", "reordered", "source", "minimum", "truncated_start"])
def test_sequence_cannot_manufacture_acceptance(change):
    values, service, first, second, _ = campaign()
    if change == "late":
        first = publish(first.spec, "2025-04-10T22:00:00+08:00")
    elif change == "reordered":
        first, second = second, first
    elif change == "source":
        values["daily-10-0"]["rows"][0]["oi"] = "99"
    elif change == "truncated_start":
        first = second
    with pytest.raises(ValueError):
        check(service, first, second, minimum=2 if change == "minimum" else 1)


@pytest.mark.parametrize("change", ["reset", "policy", "historical_version", "missing", "same_day"])
def test_continuation_rejects_reset_or_replaced_evidence(change):
    _, service, first, second, body = campaign()
    if change in {"reset", "policy", "historical_version"}:
        payload = second.spec.request.model_dump()
        if change == "reset":
            payload["initial_main"] = None
        elif change == "policy":
            payload["policy"]["confirmations"] = 1
        else:
            # Equal rows under a different version identity are not the same historical input.
            payload["daily_inputs"][0]["version_id"] = "replacement"
            forged = second.spec.model_copy(
                update={"request": ComputedRequest.model_validate(payload)}
            )
            with pytest.raises(ValueError, match="固定历史"):
                check_extension(first.spec, forged)
            return
        changed = service.build(ComputedRequest.model_validate(payload))
        with pytest.raises(ValueError, match="初始状态"):
            check_extension(first.spec, changed)
    else:
        entries = body.daily_inputs[:1] if change == "missing" else first.spec.request.daily_inputs
        body = body.model_copy(update={"daily_inputs": entries})
        with pytest.raises(ValueError):
            continuation(service, first, body)


def test_no_switch_does_not_pass_rollover_acceptance():
    values, request = evidence()
    for day in (10, 11):
        values[f"daily-{day}-1"]["rows"][0]["oi"] = "100"
    service = ComputedSources(port(values))
    first_request = ComputedRequest.model_validate(
        request.model_dump() | {"daily_inputs": request.daily_inputs[:2]}
    )
    first = publish(service.build(first_request), "2025-04-10T16:00:00+08:00")
    second = publish(
        continuation(
            service,
            first,
            ContinuationRequest(
                previous_version_id=first.id,
                daily_inputs=request.daily_inputs[2:],
                explanation="no rollover fixture",
            ),
        ),
        "2025-04-11T16:00:00+08:00",
    )
    assert check(service, first, second, minimum=0).switches == 0
    with pytest.raises(ValueError, match="切换次数不足"):
        check(service, first, second)


def test_protected_api_continuation_publication_and_sequence(tmp_path, monkeypatch):
    from datetime import datetime
    from importlib import import_module

    from fastapi.testclient import TestClient
    from sqlalchemy import create_engine

    from asterion.api.app import create_app
    from asterion.platform.config import Settings

    values, _, first, second, body = campaign()
    source_port = port(values)
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", source_port)
    )
    clock = iter([first.published_at, second.published_at])

    class PublicationClock(datetime):
        @classmethod
        def now(cls, tz=None):
            return next(clock)

    monkeypatch.setattr(
        import_module("asterion.contract_roles.plugin"), "datetime", PublicationClock
    )
    engine = create_engine(f"sqlite:///{tmp_path}/sequence.db")
    settings = Settings(
        token="sequence-api-test-token-24", data_root=tmp_path, require_account=False
    )
    try:
        with TestClient(create_app(settings, engine)) as client:
            root = "/api/v1/contract-roles/computed"
            sequence = {"version_ids": [first.id, second.id], "minimum_switches": 1}
            assert (
                client.post(
                    root + "/continue-preview", json=body.model_dump(mode="json")
                ).status_code
                == 401
            )
            assert client.post(root + "/sequence/verify", json=sequence).status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            assert client.post(
                root, json=first.spec.model_dump(mode="json")
            ).json() == first.model_dump(mode="json")
            preview = client.post(root + "/continue-preview", json=body.model_dump(mode="json"))
            assert preview.status_code == 200
            assert preview.json() == second.spec.model_dump(mode="json")
            assert client.post(root, json=preview.json()).json() == second.model_dump(mode="json")
            checked = client.post(root + "/sequence/verify", json=sequence)
            assert checked.status_code == 200 and checked.json()["switches"] == 1
            values["daily-10-0"]["rows"][0]["oi"] = "99"
            assert client.post(root + "/sequence/verify", json=sequence).status_code == 422
            assert client.get(root + "/" + second.id).json() == second.model_dump(mode="json")
    finally:
        engine.dispose()


def test_delayed_first_sample_cannot_overlap_next_days_effective_opening():
    values, request = evidence()
    for index in (0, 1):
        manifest = values[f"daily-10-{index}"]["version"]["manifest"]
        manifest["observed_at"] = manifest["available_at"] = "2025-04-11T16:00:00+08:00"
    service = ComputedSources(port(values))
    first_request = ComputedRequest.model_validate(
        request.model_dump() | {"daily_inputs": request.daily_inputs[:2]}
    )
    first = publish(service.build(first_request), "2025-04-11T17:00:00+08:00")
    # Both the delayed first sample and next day's normal sample target Friday night.
    with pytest.raises(ValueError, match="重叠"):
        continuation(
            service,
            first,
            ContinuationRequest(
                previous_version_id=first.id,
                daily_inputs=request.daily_inputs[2:],
                explanation="late sample is not a timely campaign seed",
            ),
        )
