import json
import subprocess
import sys
from pathlib import Path

import pytest

import _markdownshelf_core as core


CONTRACT_PATH = Path(__file__).parents[1] / "contracts" / "abi_probe.v1.json"
CONTRACT = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


def test_loads_a_native_extension():
    assert Path(core.__file__).suffix == ".pyd"


@pytest.mark.parametrize(
    "text",
    CONTRACT["text_samples"].values(),
    ids=CONTRACT["text_samples"].keys(),
)
def test_probe_matches_versioned_contract(text):
    assert core.probe(text=text) == {
        "contract_version": CONTRACT["contract_version"],
        "text": text,
    }


@pytest.mark.parametrize("text", [42, b"abc"])
def test_probe_rejects_non_strings(text):
    with pytest.raises(TypeError):
        core.probe(text)


def test_missing_extension_fails_in_isolated_interpreter(tmp_path):
    code = """
try:
    import _markdownshelf_core
except ModuleNotFoundError as error:
    assert error.name == "_markdownshelf_core"
else:
    raise AssertionError("Absent staging unexpectedly allowed a native import")
"""
    subprocess.run(
        [sys.executable, "-I", "-S", "-c", code],
        cwd=tmp_path,
        check=True,
        timeout=30,
    )
