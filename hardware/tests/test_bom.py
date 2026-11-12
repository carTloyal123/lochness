"""The costed BOMs cover every modelled part, and the committed CSVs are current."""

import pytest

from bom import OUT, missing, rows, units, write

UNITS = units()


@pytest.mark.parametrize("key", list(UNITS))
def test_every_part_has_a_catalog_entry(key):
    assert missing(UNITS[key]) == []


@pytest.mark.parametrize("key", list(UNITS))
def test_committed_csv_is_current(key, tmp_path, monkeypatch):
    import bom
    monkeypatch.setattr(bom, "OUT", tmp_path)
    fresh = write(key, UNITS[key]).read_text()
    assert (OUT / f"{key}.csv").read_text() == fresh, "run: uv run bom.py"


@pytest.mark.parametrize("key", list(UNITS))
def test_quantities_match_the_model(key):
    modelled = sum(int(r["Qty"]) for r in rows(key, UNITS[key])
                   if r["In model"] == "yes" and r["Category"] != "Total")
    assert modelled == len(UNITS[key].items)
