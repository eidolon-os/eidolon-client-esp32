from __future__ import annotations

import os
import stat
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
WRAPPER = REPO_ROOT / "scripts/eidolon/eidolon-esp-box-3.sh"
SETTING = "CONFIG_EIDOLON_ADMISSION_SETUP_SECRET_HEX"


def _write_overlay(path: Path, value: str, *, mode: int = 0o600) -> Path:
    path.write_text(value, encoding="utf-8")
    path.chmod(mode)
    return path


def _run_validation(path: str) -> subprocess.CompletedProcess[str]:
    command = r'''
source "$1"
configure_private_sdkconfig_overlay
write_overlay
ensure_box3_sdkconfig
idf_args
printf 'PRIVATE_BUILD_DIR=%s\n' "$BUILD_DIR"
printf 'PRIVATE_SDKCONFIG_FILE=%s\n' "$SDKCONFIG_FILE"
'''
    environment = os.environ.copy()
    environment["EIDOLON_PRIVATE_SDKCONFIG_OVERLAY"] = path
    return subprocess.run(
        ["bash", "-c", command, "bash", str(WRAPPER)],
        cwd=REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def test_private_overlay_is_path_only_and_keeps_sdkconfig_outside_repo(
    tmp_path: Path,
) -> None:
    secret = "ab" * 32
    overlay = _write_overlay(
        tmp_path / "commissioning.sdkconfig",
        f'{SETTING}="{secret}"\n',
    )
    public_build = REPO_ROOT / "build/eidolon/esp-box-3"
    before = {
        path: path.read_bytes()
        for path in public_build.glob("sdkconfig*")
        if path.is_file()
    }

    result = _run_validation(str(overlay))

    assert result.returncode == 0, result.stderr
    assert secret not in result.stdout
    assert secret not in result.stderr
    assert str(overlay) in result.stdout
    private_root = Path(f"{overlay}.build")
    private_build = private_root / "esp-box-3"
    assert stat.S_IMODE(private_root.stat().st_mode) == 0o700
    assert stat.S_IMODE(private_build.stat().st_mode) == 0o700
    assert f"-B\n{private_build}\n" in result.stdout
    assert f"-DSDKCONFIG={private_build}/sdkconfig.esp-box-3" in result.stdout
    defaults_line = next(
        line for line in result.stdout.splitlines() if line.startswith("-DSDKCONFIG_DEFAULTS=")
    )
    sealed_overlay = private_root / "sdkconfig.private.esp-box-3"
    assert defaults_line.endswith(f";{sealed_overlay}")
    assert sealed_overlay.read_text(encoding="utf-8") == f'{SETTING}="{secret}"\n'
    assert stat.S_IMODE(sealed_overlay.stat().st_mode) == 0o600
    assert before == {
        path: path.read_bytes()
        for path in public_build.glob("sdkconfig*")
        if path.is_file()
    }
    for path in public_build.glob("sdkconfig*"):
        if path.is_file():
            assert secret.encode() not in path.read_bytes()


@pytest.mark.parametrize(
    "content",
    [
        f'{SETTING}="short"\n',
        f'{SETTING}="{"AB" * 32}"\n',
        f'{SETTING}="{"ab" * 32}"\nCONFIG_UNRELATED=y\n',
        f'# comment\n{SETTING}="{"ab" * 32}"\n',
    ],
)
def test_private_overlay_rejects_weak_or_additional_content(
    tmp_path: Path, content: str
) -> None:
    overlay = _write_overlay(tmp_path / "invalid.sdkconfig", content)

    result = _run_validation(str(overlay))

    assert result.returncode != 0
    assert "must contain exactly" in result.stderr
    assert "ab" * 32 not in result.stderr


def test_private_overlay_rejects_permissions_other_than_0600(tmp_path: Path) -> None:
    overlay = _write_overlay(
        tmp_path / "readable.sdkconfig",
        f'{SETTING}="{"ab" * 32}"\n',
        mode=0o640,
    )

    result = _run_validation(str(overlay))

    assert result.returncode != 0
    assert "permissions must be 0600" in result.stderr


@pytest.mark.parametrize("mode", [0o750, 0o770, 0o777])
def test_private_overlay_rejects_shared_parent(tmp_path: Path, mode: int) -> None:
    shared_parent = tmp_path / "shared"
    shared_parent.mkdir(mode=mode)
    shared_parent.chmod(mode)
    overlay = _write_overlay(
        shared_parent / "commissioning.sdkconfig",
        f'{SETTING}="{"ab" * 32}"\n',
    )

    result = _run_validation(str(overlay))

    assert result.returncode != 0
    assert "parent permissions must be 0700" in result.stderr


def test_private_overlay_rejects_oversized_input(tmp_path: Path) -> None:
    overlay = _write_overlay(tmp_path / "oversized.sdkconfig", "x" * 257)

    result = _run_validation(str(overlay))

    assert result.returncode != 0
    assert "invalid size" in result.stderr


def test_private_overlay_rejects_symlink(tmp_path: Path) -> None:
    target = _write_overlay(
        tmp_path / "target.sdkconfig",
        f'{SETTING}="{"ab" * 32}"\n',
    )
    link = tmp_path / "link.sdkconfig"
    link.symlink_to(target)

    result = _run_validation(str(link))

    assert result.returncode != 0
    assert "regular, non-symlink" in result.stderr


def test_private_overlay_rejects_relative_path(tmp_path: Path) -> None:
    overlay = _write_overlay(
        tmp_path / "relative.sdkconfig",
        f'{SETTING}="{"ab" * 32}"\n',
    )

    result = _run_validation(os.path.relpath(overlay, REPO_ROOT))

    assert result.returncode != 0
    assert "must be an absolute path" in result.stderr


def test_private_overlay_rejects_repository_path() -> None:
    secure_parent = REPO_ROOT / "build/private-sdkconfig-overlay-parent-test"
    secure_parent.mkdir(parents=True, exist_ok=True)
    secure_parent.chmod(0o700)
    overlay = secure_parent / "private-sdkconfig-overlay-test"
    _write_overlay(overlay, f'{SETTING}="{"ab" * 32}"\n')
    try:
        result = _run_validation(str(overlay))
    finally:
        overlay.unlink(missing_ok=True)
        secure_parent.rmdir()

    assert result.returncode != 0
    assert "must be outside the repository" in result.stderr


def test_replacing_source_after_validation_cannot_change_sealed_input(
    tmp_path: Path,
) -> None:
    original_secret = "ab" * 32
    replacement_secret = "cd" * 32
    overlay = _write_overlay(
        tmp_path / "commissioning.sdkconfig",
        f'{SETTING}="{original_secret}"\n',
    )
    replacement = _write_overlay(
        tmp_path / "replacement.sdkconfig",
        f'{SETTING}="{replacement_secret}"\n',
    )
    command = r'''
source "$1"
configure_private_sdkconfig_overlay
mv "$2" "$3"
idf_args
'''
    environment = os.environ.copy()
    environment["EIDOLON_PRIVATE_SDKCONFIG_OVERLAY"] = str(overlay)

    result = subprocess.run(
        ["bash", "-c", command, "bash", str(WRAPPER), str(replacement), str(overlay)],
        cwd=REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert original_secret not in result.stdout + result.stderr
    assert replacement_secret not in result.stdout + result.stderr
    defaults_line = next(
        line for line in result.stdout.splitlines() if line.startswith("-DSDKCONFIG_DEFAULTS=")
    )
    sealed_overlay = Path(defaults_line.rsplit(";", 1)[1])
    assert sealed_overlay.read_text(encoding="utf-8") == (
        f'{SETTING}="{original_secret}"\n'
    )
