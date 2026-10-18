import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
os.environ.setdefault("PET_DATA", "/tmp/pet-bridge-test-unused")

import brain  # noqa: E402


@pytest.fixture
def brain_dir(tmp_path, monkeypatch):
    """A fresh brain folder from the templates, in a temp dir."""
    monkeypatch.setattr(brain, "DATA", str(tmp_path))
    monkeypatch.setattr(brain, "BRAIN", str(tmp_path / "brain"))
    brain.setup()
    return tmp_path / "brain"
