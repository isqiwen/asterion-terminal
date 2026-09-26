"""Cross-owner writes are atomic and cannot widen either owner's table grants."""

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.storage import Storage
from sqlalchemy import Column, Integer, MetaData, Table, select


def test_join_grants_rollback_and_readonly_boundary(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/join.db")
    meta = MetaData()
    left = Table("left_owner", meta, Column("id", Integer, primary_key=True))
    right = Table("right_owner", meta, Column("id", Integer, primary_key=True))
    meta.create_all(engine)
    a, b = Storage(engine, (left,)), Storage(engine, (right,))
    with a.begin() as outer:
        outer.execute(left.insert().values(id=1))
        with b.join(outer) as joined:
            joined.execute(right.insert().values(id=1))
            with pytest.raises(ValueError, match="granted"):
                joined.execute(left.insert().values(id=2))
        with pytest.raises(ValueError, match="closed"):
            joined.execute(select(right))
    with pytest.raises(RuntimeError), a.begin() as outer:
        with b.join(outer) as joined:
            joined.execute(right.insert().values(id=2))
        raise RuntimeError("rollback all participants")
    with b.connect() as conn:
        assert conn.execute(select(right.c.id)).scalars().all() == [1]
    with a.connect() as outer, pytest.raises(ValueError, match="read-only"), b.join(outer):
        pass
    with (
        a.begin() as outer,
        b.borrow(outer) as reader,
        pytest.raises(ValueError, match="read-only"),
        b.join(reader),
    ):
        pass
    other = create_engine(f"sqlite:///{tmp_path}/other.db")
    foreign = Storage(other, (right,))
    with (
        a.begin() as outer,
        pytest.raises(ValueError, match="another database"),
        foreign.join(outer),
    ):
        pass
    a.close()
    b.close()
    foreign.close()
    engine.dispose()
    other.dispose()
