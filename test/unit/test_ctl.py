"""
The control logic against a plain directory standing in for configfs; no
module, no root.
"""

import pytest

from lsvtest import ctl


@pytest.fixture
def roots(tmp_path):
    configfs = tmp_path / "config"
    (configfs / ctl.MODULE_NAME).mkdir(parents=True)
    dev = tmp_path / "dev"
    dev.mkdir()
    return configfs, dev


def make_device(roots, **kw):
    configfs, dev = roots
    spec = ctl.DeviceSpec(name="lsv0", backing="/dev/ram0", index_ds="ht", **kw)
    return ctl.Device(spec, configfs_root=configfs, dev_root=dev)


def test_create_writes_attrs_then_create_new(roots, monkeypatch):
    written = []
    real = ctl.write_attr
    monkeypatch.setattr(
        ctl, "write_attr", lambda p, v: (written.append((p.name, v)), real(p, v))
    )

    dev = make_device(roots, cell_size=8192, dedup=True)
    dev.create(timeout=0)

    assert written == [
        ("backing_path", "/dev/ram0"),
        ("index_ds", "ht"),
        ("cell_size", 8192),
        ("dedup", 1),
        ("create_new", 1),
    ]
    assert (dev.item / "backing_path").read_text() == "/dev/ram0\n"
    assert dev.created


def test_segment_size_written_only_when_set(roots):
    assert "segment_size" not in make_device(roots).spec.attrs()
    assert make_device(roots, segment_size=1 << 21).spec.attrs()["segment_size"] == (
        1 << 21
    )


def test_create_twice_fails(roots):
    make_device(roots).create(timeout=0)
    with pytest.raises(ctl.LsvError, match="already exists"):
        make_device(roots).create(timeout=0)


def test_failed_attr_tears_the_item_down(roots, monkeypatch):
    def refuse(path, value):
        if path.name == "index_ds":
            raise ctl.LsvError("index_ds = 'zz': Invalid argument")

    destroyed = []
    monkeypatch.setattr(ctl, "write_attr", refuse)
    monkeypatch.setattr(ctl.Device, "destroy", lambda self: destroyed.append(self))

    dev = make_device(roots)
    with pytest.raises(ctl.LsvError, match="Invalid argument"):
        dev.create(timeout=0)
    assert destroyed == [dev]


def test_write_attr_reports_errno(tmp_path):
    with pytest.raises(ctl.LsvError, match="create_new = 1"):
        ctl.write_attr(tmp_path / "missing" / "create_new", 1)


def test_wait_node(roots):
    dev = make_device(roots)
    with pytest.raises(ctl.LsvError, match="did not appear"):
        dev.wait_node(0.1)
    dev.path.touch()
    dev.wait_node(0.1)


def test_device_names(roots):
    configfs, _ = roots
    for name in ("lsv1", "lsv0"):
        (configfs / ctl.MODULE_NAME / name).mkdir()
    assert ctl.device_names(configfs) == ["lsv0", "lsv1"]
    assert ctl.device_names(configfs / "nowhere") == []


def test_open_backing_existing_and_unknown():
    assert ctl.open_backing("/dev/nvme0n1").path == "/dev/nvme0n1"
    with pytest.raises(ctl.LsvError, match="unknown backing"):
        ctl.open_backing("tmpfs")
