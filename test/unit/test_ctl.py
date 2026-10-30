"""
The control logic against a plain directory standing in for /sys; no
module, no root.
"""

import pytest

from lsvtest import ctl


@pytest.fixture
def roots(tmp_path):
    sys_root = tmp_path / "sys"
    ctl.params_dir(sys_root).mkdir(parents=True)
    dev = tmp_path / "dev"
    dev.mkdir()
    return sys_root, dev


def make_device(roots, name="lsvbd1", ds="ht"):
    sys_root, dev = roots
    spec = ctl.DeviceSpec(name=name, backing="/dev/ram0", index_ds=ds)
    return ctl.Device(spec, sys_root=sys_root, dev_root=dev)


def set_listing(roots, *rows):
    sys_root, _ = roots
    text = "".join(f"{i}. {name} -> ram0\n" for i, name in enumerate(rows, 1))
    (ctl.params_dir(sys_root) / "get_vbd_names").write_text(text)


def test_create_picks_the_index_then_redirects(roots, monkeypatch):
    written = []
    real = ctl.write_attr
    monkeypatch.setattr(
        ctl, "write_attr", lambda p, v: (written.append((p.name, v)), real(p, v))
    )

    dev = make_device(roots, name="lsvbd3", ds="sl")
    dev.create(timeout=0)

    assert written == [
        ("set_data_structure", "sl"),
        ("set_redirect_bd", "3 /dev/ram0"),
    ]
    assert (dev.params / "set_redirect_bd").read_text() == "3 /dev/ram0\n"


def test_name_must_carry_an_index():
    with pytest.raises(ctl.LsvError, match="lsvbd<N>"):
        ctl.DeviceSpec("lsv0", "/dev/ram0", "ht").attrs()


def test_unknown_index_is_refused(roots):
    with pytest.raises(ctl.LsvError, match="unknown index"):
        make_device(roots, ds="zz").create(timeout=0)


def test_create_twice_fails(roots):
    set_listing(roots, "lsvbd1")
    with pytest.raises(ctl.LsvError, match="already exists"):
        make_device(roots).create(timeout=0)


def test_device_names_and_created(roots):
    sys_root, _ = roots
    assert ctl.device_names(sys_root) == []
    set_listing(roots, "lsvbd2", "lsvbd1")
    assert ctl.device_names(sys_root) == ["lsvbd1", "lsvbd2"]
    assert make_device(roots).created
    assert ctl.device_names(sys_root / "nowhere") == []


def test_destroy_deletes_by_listing_position(roots):
    set_listing(roots, "lsvbd2", "lsvbd1")
    dev = make_device(roots)
    dev.destroy()
    assert (dev.params / "delete_bd").read_text() == "2\n"


def test_destroy_of_a_missing_device_is_a_no_op(roots):
    dev = make_device(roots)
    dev.destroy()
    assert not (dev.params / "delete_bd").exists()


def test_write_attr_reports_errno(tmp_path):
    with pytest.raises(ctl.LsvError, match="delete_bd = 1"):
        ctl.write_attr(tmp_path / "missing" / "delete_bd", 1)


def test_wait_node(roots):
    dev = make_device(roots)
    with pytest.raises(ctl.LsvError, match="did not appear"):
        dev.wait_node(0.1)
    dev.path.touch()
    dev.wait_node(0.1)


def test_module_loaded(roots):
    sys_root, _ = roots
    assert ctl.module_loaded(sys_root)
    assert not ctl.module_loaded(sys_root / "nowhere")


def test_open_backing_existing_and_unknown():
    assert ctl.open_backing("/dev/nvme0n1").path == "/dev/nvme0n1"
    with pytest.raises(ctl.LsvError, match="unknown backing"):
        ctl.open_backing("tmpfs")
