"""
Тесты корректности фреймворка (запуск: cd ton_quant && python -m pytest -q tests).
Главное — отсутствие look-ahead: признаки, сигналы и режимы в момент T не должны
меняться, если отрезать все данные после T.
"""
import os
import sys

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from backtest import funding_per_bar, run_backtest  # noqa: E402
from config import CostModel, RiskModel  # noqa: E402
from features import build_features  # noqa: E402
from regime import HMMRegime, regime_inputs  # noqa: E402
from signals import STRATEGIES, generate, param_grid  # noqa: E402
from synthetic import make_panel  # noqa: E402
from validation import deflated_sharpe, probabilistic_sharpe, purged_kfold  # noqa: E402


@pytest.fixture(scope="module")
def panel():
    return make_panel(n_days=260, mode="null")


def _truncate(panel, T):
    out = dict(panel)
    for k in ("perp", "btc", "eth"):
        out[k] = panel[k].loc[:T]
    out["funding"] = panel["funding"].loc[:T + pd.Timedelta("1h")]   # funding известен на close бара T
    out["metrics"] = panel["metrics"].loc[:T + pd.Timedelta("1h")]
    out["premium"] = panel["premium"].loc[:T]
    return out


def _assert_frame_prefix_equal(full, part, cols):
    a, b = full.loc[part.index, cols], part[cols]
    for c in cols:
        x, y = a[c].to_numpy(dtype=float), b[c].to_numpy(dtype=float)
        both_nan = np.isnan(x) & np.isnan(y)
        assert np.all(both_nan | np.isclose(x, y, rtol=1e-7, atol=1e-10)), f"look-ahead в признаке {c}"


def test_features_no_lookahead(panel):
    full = build_features(panel)
    T = panel["perp"].index[int(len(panel["perp"]) * 0.6)]
    part = build_features(_truncate(panel, T))
    cols = [c for c in full.columns if c not in ("gap_before",)]
    _assert_frame_prefix_equal(full, part, cols)


def test_signals_no_lookahead(panel):
    full_f = build_features(panel)
    T = panel["perp"].index[int(len(panel["perp"]) * 0.6)]
    p2 = _truncate(panel, T)
    part_f = build_features(p2)
    reg_full = pd.Series("trend", index=full_f.index)
    for name in STRATEGIES:
        prm = param_grid(name)[0]
        s1 = generate(name, full_f, panel["perp"]["close"], reg_full, prm)
        s2 = generate(name, part_f, p2["perp"]["close"], reg_full.loc[:T], prm)
        for c in ("long_entry", "short_entry", "long_exit", "short_exit"):
            assert (s1.loc[s2.index, c] == s2[c]).all(), f"{name}.{c}: look-ahead"


def test_hmm_filter_is_causal(panel):
    f = build_features(panel)
    x = regime_inputs(f)
    n = len(x)
    hmm = HMMRegime().fit(x.iloc[: n // 2])
    p_full = hmm.filter_proba(x)
    p_part = hmm.filter_proba(x.iloc[: int(n * 0.7)])
    a = p_full.loc[p_part.index].to_numpy(float)
    b = p_part.to_numpy(float)
    m = ~np.isnan(a) & ~np.isnan(b)
    assert np.allclose(a[m], b[m], atol=1e-9)


def _toy(n=50, px=100.0):
    idx = pd.date_range("2025-01-01", periods=n, freq="1h", tz="UTC")
    o = np.full(n, px)
    df = pd.DataFrame({"open": o, "high": o * 1.001, "low": o * 0.999, "close": o, "volume": 1.0,
                       "gap_before": False}, index=idx)
    sig = pd.DataFrame({"long_entry": False, "short_entry": False, "long_exit": False, "short_exit": False,
                        "stop_atr": 10.0, "tp_atr": np.nan, "max_hold": 1000, "size_mult": 1.0,
                        "confluence": 0.0}, index=idx)
    return df, sig


def test_entry_next_open_and_costs():
    df, sig = _toy()
    df.loc[df.index[6], "open"] = 101.0
    sig.loc[sig.index[5], "long_entry"] = True
    costs = CostModel(taker_fee=0.001, slippage_bps=10)
    r = run_backtest(df, sig, pd.Series(1.0, df.index), "1h", None, costs, RiskModel())
    t = r.trades.iloc[0]
    assert t.entry_time == df.index[6]                       # сигнал на баре 5 -> вход на open бара 6
    assert np.isclose(t.entry, 101.0 * (1 + 10e-4))          # проскальзывание против нас
    assert t.fees > 0


def test_funding_sign_long_pays_positive():
    df, sig = _toy()
    sig.loc[sig.index[0], "long_entry"] = True
    f = pd.Series([0.001], index=[df.index[10] + pd.Timedelta("1h")])  # в момент закрытия бара 10
    fb = funding_per_bar(df.index, "1h", f)
    assert fb[10] == 0.001 and fb.sum() == 0.001
    r = run_backtest(df, sig, pd.Series(1.0, df.index), "1h", f, CostModel(taker_fee=0, slippage_bps=0),
                     RiskModel())
    assert r.trades.funding.sum() < 0                          # лонг платит при положительной ставке


def test_liquidation_before_far_stop():
    df, sig = _toy()
    sig.loc[sig.index[0], "long_entry"] = True
    sig["stop_atr"] = 60.0                                     # стоп дальше цены ликвидации
    df.loc[df.index[20], "low"] = 50.0                         # обвал на 50%
    risk = RiskModel(leverage=3.0, maint_margin_rate=0.01, risk_per_trade=0.5)
    r = run_backtest(df, sig, pd.Series(1.0, df.index), "1h", None, CostModel(), risk)
    assert r.trades.iloc[0].reason == "liquidation"
    liq = 100 * (1 + 3e-4) * (1 - (1 / 3 - 0.01))
    assert r.trades.iloc[0].exit == pytest.approx(liq, rel=1e-6)


def test_stop_beats_tp_in_same_bar():
    df, sig = _toy()
    sig.loc[sig.index[0], "long_entry"] = True
    sig["stop_atr"], sig["tp_atr"] = 1.0, 1.0
    df.loc[df.index[5], ["high", "low"]] = [102.0, 98.0]
    r = run_backtest(df, sig, pd.Series(1.0, df.index), "1h", None, CostModel(), RiskModel())
    assert r.trades.iloc[0].reason == "stop"


def test_purged_kfold_no_overlap():
    t = pd.date_range("2025-01-01", periods=500, freq="1h", tz="UTC")
    le = t + pd.Timedelta("24h")
    for tr, te in purged_kfold(t, le, k=5, embargo_frac=0.02):
        t0, t1 = t[te[0]], le[te].max()
        assert not ((t[tr] <= t1) & (le[tr] >= t0)).any()


def test_dsr_reduces_to_psr_single_trial():
    rng = np.random.default_rng(0)
    r = pd.Series(rng.normal(0.001, 0.02, 500))
    d = deflated_sharpe(r, np.array([0.05]))
    from scipy.stats import kurtosis, skew
    x = r.to_numpy()
    psr = probabilistic_sharpe(x.mean() / x.std(), 0.0, len(r), skew(r), kurtosis(r, fisher=False))
    assert d["DSR"] == pytest.approx(psr)
