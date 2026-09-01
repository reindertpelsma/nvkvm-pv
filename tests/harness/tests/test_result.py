"""Result-vocabulary rules: PASS/FAIL/SKIP/UNTESTED, the comparison-first
data model, and "a result that did not run must never render as a pass or as
1.00". Runs entirely without a GPU -- everything here is built by hand."""

import pytest

from result import Comparison, Observation, Report, Status, ToleranceBand


def obs(status, detail="detail", value=None, label="") -> Observation:
    return Observation(status=status, detail=detail, value=value, label=label)


# --- Observation -------------------------------------------------------------


def test_observation_requires_nonempty_detail():
    with pytest.raises(ValueError):
        Observation(status=Status.PASS, detail="")
    with pytest.raises(ValueError):
        Observation(status=Status.PASS, detail="   ")


def test_observation_requires_a_real_status():
    with pytest.raises(TypeError):
        Observation(status="PASS", detail="not a Status instance")  # type: ignore[arg-type]


# --- ToleranceBand -------------------------------------------------------------


def test_tolerance_band_rejects_unknown_direction():
    with pytest.raises(ValueError):
        ToleranceBand("sideways", 1.0)


def test_tolerance_band_higher_is_better():
    band = ToleranceBand("higher_is_better", 0.90)
    assert band.passes(0.90) is True
    assert band.passes(0.95) is True
    assert band.passes(0.89) is False


def test_tolerance_band_lower_is_better():
    band = ToleranceBand("lower_is_better", 4.0)
    assert band.passes(4.0) is True
    assert band.passes(1.85) is True
    assert band.passes(4.01) is False


# --- Comparison: degenerate (no baseline) case --------------------------------


def test_degenerate_comparison_has_no_ratio():
    c = Comparison(name="x", tier="medium", target=obs(Status.PASS, "42 GFLOP/s", value=42.0))
    assert c.baseline is None
    assert c.ratio is None
    assert "no baseline machine configured" in c.detail()


def test_degenerate_comparison_status_passes_through_target():
    for status in (Status.PASS, Status.FAIL, Status.SKIP, Status.UNTESTED):
        c = Comparison(name="x", tier="small", target=obs(status, f"detail for {status}"))
        assert c.status is status


def test_missing_baseline_never_renders_as_1_00_or_a_pass():
    """The specific failure mode this project got burned by: a missing run
    must never look like parity. A degenerate (no-baseline) PASS reports the
    target's own status, not a fabricated ratio."""
    c = Comparison(name="x", tier="medium", target=obs(Status.PASS, "14.2 GB/s", value=14.2), band=ToleranceBand("higher_is_better", 0.80))
    assert c.ratio is None
    assert c.to_dict()["ratio"] is None


# --- Comparison: two-sided ------------------------------------------------


def test_two_sided_within_band_passes():
    band = ToleranceBand("higher_is_better", 0.80)
    c = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.PASS, "14.2 GB/s", value=14.2, label="host"),
        target=obs(Status.PASS, "13.4 GB/s", value=13.4, label="guest"),
        band=band,
    )
    assert c.ratio == pytest.approx(13.4 / 14.2)
    assert c.status is Status.PASS


def test_two_sided_outside_band_fails():
    band = ToleranceBand("higher_is_better", 0.80)
    c = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.PASS, "14.2 GB/s", value=14.2, label="host"),
        target=obs(Status.PASS, "5.0 GB/s", value=5.0, label="guest"),
        band=band,
    )
    assert c.status is Status.FAIL


def test_two_sided_target_fail_outranks_everything():
    band = ToleranceBand("higher_is_better", 0.80)
    c = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.PASS, "14.2 GB/s", value=14.2, label="host"),
        target=obs(Status.FAIL, "correctness FAIL: byte mismatch", label="guest"),
        band=band,
    )
    assert c.status is Status.FAIL


def test_two_sided_baseline_fail_also_fails():
    band = ToleranceBand("higher_is_better", 0.80)
    c = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.FAIL, "host probe crashed", label="host"),
        target=obs(Status.PASS, "13.4 GB/s", value=13.4, label="guest"),
        band=band,
    )
    assert c.status is Status.FAIL


def test_two_sided_missing_value_on_either_side_is_untested_never_a_ratio():
    band = ToleranceBand("higher_is_better", 0.80)

    baseline_missing = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.SKIP, "no GPU on baseline machine", label="host"),
        target=obs(Status.PASS, "13.4 GB/s", value=13.4, label="guest"),
        band=band,
    )
    assert baseline_missing.ratio is None
    assert baseline_missing.status is Status.UNTESTED

    target_missing = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.PASS, "14.2 GB/s", value=14.2, label="host"),
        target=obs(Status.UNTESTED, "probe timed out on guest", label="guest"),
        band=band,
    )
    assert target_missing.ratio is None
    assert target_missing.status is Status.UNTESTED


def test_zero_baseline_value_does_not_divide_by_zero():
    band = ToleranceBand("higher_is_better", 0.80)
    c = Comparison(
        name="bw", tier="medium",
        baseline=obs(Status.PASS, "0 GB/s", value=0.0, label="host"),
        target=obs(Status.PASS, "13.4 GB/s", value=13.4, label="guest"),
        band=band,
    )
    assert c.ratio is None
    assert c.status is Status.UNTESTED


# --- Report / exit code -------------------------------------------------------


def test_exit_code_ordering_matches_validate_sh():
    # FAIL outranks UNTESTED outranks a clean pass, regardless of how many
    # other checks passed -- same ordering as tests/validate.sh's verdict().
    report = Report(tier="medium")
    report.add(Comparison(name="a", tier="medium", target=obs(Status.PASS, "ok")))
    report.add(Comparison(name="b", tier="medium", target=obs(Status.UNTESTED, "timed out")))
    report.add(Comparison(name="c", tier="medium", target=obs(Status.FAIL, "broke")))
    assert report.exit_code() == 1
    assert report.verdict() == "FAIL"


def test_exit_code_untested_without_fail():
    report = Report(tier="medium")
    report.add(Comparison(name="a", tier="medium", target=obs(Status.PASS, "ok")))
    report.add(Comparison(name="b", tier="medium", target=obs(Status.UNTESTED, "timed out")))
    assert report.exit_code() == 3
    assert report.verdict() == "CANNOT VALIDATE"


def test_exit_code_skip_never_moves_it():
    report = Report(tier="medium")
    report.add(Comparison(name="a", tier="medium", target=obs(Status.PASS, "ok")))
    report.add(Comparison(name="b", tier="medium", target=obs(Status.SKIP, "no GPU here")))
    assert report.exit_code() == 0
    assert report.verdict() == "PASS"


def test_exit_code_clean_pass():
    report = Report(tier="small")
    report.add(Comparison(name="a", tier="small", target=obs(Status.PASS, "ok")))
    assert report.exit_code() == 0


def test_report_json_round_trips_and_never_hides_untested(tmp_path):
    report = Report(tier="medium")
    report.add(Comparison(name="a", tier="medium", target=obs(Status.UNTESTED, "harness broke")))
    report.finish()
    path = tmp_path / "out.json"
    report.write_json(path)

    import json

    payload = json.loads(path.read_text())
    assert payload["exit_code"] == 3
    assert payload["verdict"] == "CANNOT VALIDATE"
    assert payload["comparisons"][0]["status"] == "UNTESTED"
