import pytest

from lsvtest import spec


def doc(**overrides):
    base = {
        "name": "t",
        "matrix": {"ds": ["ht", "sl"], "rw": ["randwrite", "randrw:70"], "bs": ["4k"]},
    }
    for section, values in overrides.items():
        base.setdefault(section, {}).update(values)
    return base


@pytest.mark.parametrize(
    "value, expected",
    [("4k", 4096), ("16K", 16384), ("1m", 1 << 20), ("2GiB", 2 << 30), (512, 512)],
)
def test_parse_size(value, expected):
    assert spec.parse_size(value) == expected


def test_parse_size_rejects_garbage():
    with pytest.raises(spec.SpecError):
        spec.parse_size("4 pages")


@pytest.mark.parametrize(
    "value, expected",
    [
        ("randread", ("randread", None)),
        ("randrw", ("randrw", 50)),
        ("randrw:70", ("randrw", 70)),
        ("rw:0", ("rw", 0)),
    ],
)
def test_parse_rw(value, expected):
    assert spec.parse_rw(value) == expected


@pytest.mark.parametrize("value", ["trim", "randrw:101", "randread:50"])
def test_parse_rw_rejects(value):
    with pytest.raises(spec.SpecError):
        spec.parse_rw(value)


def test_matrix_is_the_cross_product():
    points = spec.from_dict(doc()).points()
    assert len(points) == 4
    assert {(p.ds, p.rw, p.rwmixread) for p in points} == {
        ("ht", "randwrite", None),
        ("ht", "randrw", 70),
        ("sl", "randwrite", None),
        ("sl", "randrw", 70),
    }
    assert all(p.target == "lsv" and p.numjobs == p.iodepth == 1 for p in points)


def test_scalar_defaults_from_sections():
    exp = spec.from_dict(doc(fio={"iodepth": 32, "numjobs": 4}))
    assert {(p.numjobs, p.iodepth) for p in exp.points()} == {(4, 32)}


def test_matrix_overrides_section_default():
    exp = spec.from_dict(doc(fio={"iodepth": 32}, matrix={"iodepth": [1, 8]}))
    assert {p.iodepth for p in exp.points()} == {1, 8}


def test_concurrency_pairs_are_zipped_not_crossed():
    exp = spec.from_dict(doc(matrix={"concurrency": [[1, 4], [8, 32]]}))
    pairs = {(p.numjobs, p.iodepth) for p in exp.points()}
    assert pairs == {(1, 4), (8, 32)}
    assert len(exp.points()) == 2 * 2 * 2


def test_baseline_has_one_point_per_workload():
    exp = spec.from_dict(doc(run={"baseline": True}))
    raw = [p for p in exp.points() if p.target == spec.RAW]
    # ds does not exist below lsbdd, so the indexes collapse
    assert len(raw) == 2
    assert all(p.ds == spec.RAW for p in raw)


def test_point_keys_are_unique():
    exp = spec.from_dict(
        doc(run={"baseline": True}, matrix={"numjobs": [1, 8], "iodepth": [1, 32]})
    )
    keys = [p.key() for p in exp.points()]
    assert len(keys) == len(set(keys))


def test_read_share():
    def point(rw):
        return spec.from_dict(doc(matrix={"rw": [rw]})).points()[0]

    assert point("randread").read_share == 1.0
    assert not point("randread").writes
    assert point("randwrite").read_share == 0.0
    assert point("randrw:70").read_share == pytest.approx(0.7)


@pytest.mark.parametrize(
    "overrides, message",
    [
        ({"matrix": {"blocksize": ["4k"]}}, "unknown matrix keys"),
        ({"matrix": {"ds": "ht"}}, "non-empty list"),
        ({"run": {"precondition": "sometimes"}}, "precondition"),
        ({"run": {"region": 1.5}}, "region"),
        ({"run": {"repeats": 0}}, "repeats"),
        ({"matrix": {"ds": ["ht", "lsm"]}}, "unknown ds"),
        ({"matrix": {"cell_size": ["4k"]}}, "not supported by the lsbdd driver"),
        ({"device": {"dedup": True}}, "not supported by the lsbdd driver"),
        (
            {"matrix": {"concurrency": [[1, 4]], "numjobs": [1]}},
            "drop matrix.numjobs",
        ),
        ({"matrix": {"concurrency": [[1, 4, 8]]}}, "pairs"),
        ({"matrix": {"concurrency": [[0, 4]]}}, "pairs"),
    ],
)
def test_validation(overrides, message):
    with pytest.raises(spec.SpecError, match=message):
        spec.from_dict(doc(**overrides))


def test_missing_required_axis():
    with pytest.raises(spec.SpecError, match="bs is not set"):
        spec.from_dict({"matrix": {"ds": ["ht"], "rw": ["read"]}})


def test_shipped_experiments_load():
    from pathlib import Path

    files = sorted((Path(__file__).parents[1] / "experiments").glob("*.toml"))
    assert files
    for path in files:
        assert spec.load(path).points()
