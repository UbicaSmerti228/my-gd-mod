"""
signals.py — правила входа/выхода. Сигнал формируется на ЗАКРЫТИИ бара t,
исполнение — на ОТКРЫТИИ бара t+1 (это делает backtest.py).

Каждая стратегия возвращает DataFrame с колонками:
  long_entry, short_entry, long_exit, short_exit : bool
  stop_atr  : множитель ATR для стопа (от цены входа)
  tp_atr    : множитель ATR для тейка (NaN = без тейка)
  max_hold  : тайм-стоп в барах
  size_mult : множитель размера (например, 0.5 в high_vol)
  confluence: целочисленный скор совпавших независимых факторов (-8..+8)

Сетки параметров НАМЕРЕННО маленькие: каждая лишняя комбинация увеличивает
число испытаний и штраф в Deflated Sharpe Ratio.
"""
from __future__ import annotations

import itertools

import numpy as np
import pandas as pd

FACTORS = ("f_trend", "f_htf", "f_mom", "f_flow", "f_funding", "f_oi", "f_btc", "f_rs")


def _col(f: pd.DataFrame, name: str) -> pd.Series:
    return f[name] if name in f.columns else pd.Series(np.nan, index=f.index)


def confluence(f: pd.DataFrame) -> pd.DataFrame:
    """Голоса независимых факторов: +1 за лонг, -1 за шорт, 0 — нет мнения/нет данных."""
    v = pd.DataFrame(index=f.index)
    e1, e2 = _col(f, "ema_20_50"), _col(f, "ema_50_200")
    v["f_trend"] = np.where(e1 == e2, e1, 0.0)                    # тренд (согласованный стек EMA)
    v["f_htf"] = _col(f, "h4_trend")                                # тренд старшего ТФ
    v["f_mom"] = np.sign(_col(f, "macd_hist"))                      # импульс
    flow = _col(f, "cvd_slope").fillna(_col(f, "obv_slope"))
    v["f_flow"] = np.where(flow.abs() > 0.5, np.sign(flow), 0.0)    # поток ордеров
    fz = _col(f, "funding_z")
    v["f_funding"] = np.where(fz.abs() > 1.5, -np.sign(fz), 0.0)    # контрарный funding
    q = _col(f, "oi_px_quadrant")
    v["f_oi"] = np.select([q == 3, q == -1], [1.0, -1.0], 0.0)     # цена↑OI↑ новые лонги / цена↓OI↑ новые шорты
    v["f_btc"] = _col(f, "btc_trend")                               # тренд BTC
    v["f_rs"] = np.sign(_col(f, "rs_btc"))                          # относит. сила TON/BTC
    v = v.fillna(0.0)
    v["confluence"] = v[list(FACTORS)].sum(axis=1)
    return v


def _base(f: pd.DataFrame) -> pd.DataFrame:
    s = pd.DataFrame(index=f.index)
    for c in ("long_entry", "short_entry", "long_exit", "short_exit"):
        s[c] = False
    s["stop_atr"], s["tp_atr"], s["max_hold"], s["size_mult"] = 2.0, np.nan, 24 * 7, 1.0
    return s


def _regime_ok(regime, allowed):
    """Базовые стратегии по умолчанию режим-агностичны (allowed=None); regime_switch
    передаёт allowed явно — так сравнивается «с учётом режима» против «без»."""
    if regime is None or not allowed:
        return True
    return regime.isin(allowed)


def _rise(cond: pd.Series) -> pd.Series:
    """Событие: условие стало истинным на этом баре (защита от повторного входа сразу после стопа)."""
    c = cond.fillna(False).astype(bool)
    return c & ~c.shift(1, fill_value=False)


# ------------------------------------------------------------------ стратегии
def trend_following(f, regime, p):
    """EMA-стек + Supertrend + ADX, вход при confluence >= min_conf."""
    s, cf = _base(f), confluence(f)
    up = (f["ema_20_50"] > 0) & (f["supertrend"] > 0) & (f["adx"] > p["adx_thr"])
    dn = (f["ema_20_50"] < 0) & (f["supertrend"] < 0) & (f["adx"] > p["adx_thr"])
    ok = _regime_ok(regime, p.get("regimes"))
    s["long_entry"] = _rise(up & ok & (cf["confluence"] >= p["min_conf"]))
    s["short_entry"] = _rise(dn & ok & (cf["confluence"] <= -p["min_conf"]))
    s["long_exit"] = f["supertrend"] < 0
    s["short_exit"] = f["supertrend"] > 0
    s["stop_atr"], s["tp_atr"], s["max_hold"] = p["stop_atr"], np.nan, p.get("max_hold", 24 * 14)
    s["confluence"] = cf["confluence"]
    return s


def mean_reversion(f, regime, p):
    """Bollinger %b + RSI экстремумы во флэте; выход на средней линии."""
    s, cf = _base(f), confluence(f)
    ok = _regime_ok(regime, p.get("regimes"))
    s["long_entry"] = _rise((f["bb_pctb"] < 0) & (f["rsi"] < p["rsi_lo"]) & ok)
    s["short_entry"] = _rise((f["bb_pctb"] > 1) & (f["rsi"] > 100 - p["rsi_lo"]) & ok)
    s["long_exit"] = f["bb_pctb"] >= 0.5
    s["short_exit"] = f["bb_pctb"] <= 0.5
    s["stop_atr"], s["tp_atr"], s["max_hold"] = p["stop_atr"], np.nan, p.get("max_hold", 48)
    s["confluence"] = cf["confluence"]
    return s


def squeeze_breakout(f, regime, p):
    """Выход из «сжатия» (BB внутри Keltner) + закрытие за каналом Keltner."""
    s, cf = _base(f), confluence(f)
    rel = f["squeeze"].rolling(3).max().shift(1).fillna(0) > 0   # сжатие было в последние 3 бара
    s["long_entry"] = _rise(rel & (f["squeeze"] == 0) & (f["close_"] > f["kc_up"]))
    s["short_entry"] = _rise(rel & (f["squeeze"] == 0) & (f["close_"] < f["kc_lo"]))
    s["long_exit"] = f["close_"] < f["kc_mid"]
    s["short_exit"] = f["close_"] > f["kc_mid"]
    s["stop_atr"], s["tp_atr"], s["max_hold"] = p["stop_atr"], p.get("tp_atr", np.nan), p["hold"]
    s["confluence"] = cf["confluence"]
    return s


def funding_contrarian(f, regime, p):
    """Перегрев толпы: экстремальный funding z + ослабление импульса -> против толпы."""
    s, cf = _base(f), confluence(f)
    fz = f.get("funding_z")
    if fz is None or fz.isna().all():
        s["confluence"] = cf["confluence"]
        return s
    stall_up = f["macd_hist"] < f["macd_hist"].shift(1)
    stall_dn = f["macd_hist"] > f["macd_hist"].shift(1)
    s["short_entry"] = _rise((fz > p["z_thr"]) & stall_up)
    s["long_entry"] = _rise((fz < -p["z_thr"]) & stall_dn)
    s["stop_atr"], s["tp_atr"], s["max_hold"] = p["stop_atr"], np.nan, p["hold"]
    s["confluence"] = cf["confluence"]
    return s


def regime_switch(f, regime, p):
    """Тренд-следование в режиме trend, возврат к среднему во флэте, в high_vol — вне рынка
    (или уменьшенный размер hv_size)."""
    if regime is None:
        raise ValueError("regime_switch требует ряд режимов")
    t = trend_following(f, regime, {**p, "regimes": ("trend",)})
    m = mean_reversion(f, regime, {**p, "regimes": ("range",)})
    s = _base(f)
    s["long_entry"] = t["long_entry"] | m["long_entry"]
    s["short_entry"] = t["short_entry"] | m["short_entry"]
    in_t = regime == "trend"
    s["long_exit"] = np.where(in_t, t["long_exit"], m["long_exit"] | (regime == "high_vol"))
    s["short_exit"] = np.where(in_t, t["short_exit"], m["short_exit"] | (regime == "high_vol"))
    s["stop_atr"] = np.where(in_t, t["stop_atr"], m["stop_atr"])
    s["max_hold"] = np.where(in_t, t["max_hold"], m["max_hold"])
    s["size_mult"] = np.where(regime == "high_vol", p.get("hv_size", 0.0), 1.0)
    s["confluence"] = t["confluence"]
    return s


STRATEGIES = {
    "trend": (trend_following, {"adx_thr": [20, 25, 30], "stop_atr": [2.0, 3.0], "min_conf": [2, 4]}),
    "meanrev": (mean_reversion, {"rsi_lo": [25, 30, 35], "stop_atr": [1.5, 2.5]}),
    "squeeze": (squeeze_breakout, {"hold": [12, 24, 48], "stop_atr": [1.5, 2.5]}),
    "funding": (funding_contrarian, {"z_thr": [1.5, 2.0, 2.5], "hold": [24, 48], "stop_atr": [2.0, 3.0]}),
    "regime_switch": (regime_switch, {"adx_thr": [20, 25], "stop_atr": [2.0, 3.0], "min_conf": [2, 4],
                                      "rsi_lo": [30]}),
}


def param_grid(name: str) -> list[dict]:
    grid = STRATEGIES[name][1]
    keys = list(grid)
    return [dict(zip(keys, v)) for v in itertools.product(*(grid[k] for k in keys))]


def generate(name: str, f: pd.DataFrame, close: pd.Series, regime: pd.Series | None, params: dict) -> pd.DataFrame:
    fn = STRATEGIES[name][0]
    f = f.assign(close_=close)
    sig = fn(f, regime, params)  # regime=None -> стратегии работают без фильтра режима
    for c in ("long_entry", "short_entry", "long_exit", "short_exit"):
        sig[c] = sig[c].fillna(False).astype(bool)
    return sig
