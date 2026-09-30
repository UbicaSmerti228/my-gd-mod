"""
synthetic.py — СИНТЕТИЧЕСКИЕ данные ТОЛЬКО для проверки самого фреймворка.
Никакие цифры, полученные на них, не являются выводами о рынке TON.

  mode="null"    — случайное блуждание с режимами волатильности и толстыми хвостами;
                   funding/OI/L-S НЕ несут информации о будущем. Правильный фреймворк
                   должен сказать «edge не найден» (контроль ложноположительных).
  mode="planted" — то же, но в доходность «вшит» слабый контрарный эффект funding
                   (высокий funding z -> отрицательный дрейф). Фреймворк должен его
                   обнаружить в research.test_funding_contrarian (контроль мощности).
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from config import SEED


def make_panel(n_days: int = 900, mode: str = "null", seed: int = SEED, start: str = "2024-01-01",
               planted_strength: float = 0.08) -> dict:
    rng = np.random.default_rng(seed)
    n = n_days * 24
    idx = pd.date_range(start, periods=n, freq="1h", tz="UTC")
    # --- режимы волатильности (марковская цепь)
    P = np.array([[0.995, 0.004, 0.001], [0.006, 0.990, 0.004], [0.010, 0.020, 0.970]])
    vols = np.array([0.006, 0.011, 0.025])       # часовая σ
    s = np.zeros(n, int)
    for t in range(1, n):
        s[t] = rng.choice(3, p=P[s[t - 1]])
    sig = vols[s]
    # --- BTC и TON (β≈1.2 к BTC, без lead-lag)
    eps_b = rng.standard_t(4, n) / np.sqrt(2)
    rb = 0.6 * sig * eps_b
    idio = sig * 0.7 * rng.standard_t(4, n) / np.sqrt(2)
    # --- funding: AR(1) латентный «перегрев» x_t, ставка = 1e-4 + 2e-4*x (в 8h-точках)
    x = np.zeros(n)
    for t in range(1, n):
        x[t] = 0.995 * x[t - 1] + 0.1 * rng.standard_normal()
    x = x / x.std()
    drift = np.zeros(n)
    if mode == "planted":
        # ВНИМАНИЕ: сигнал x_{t-1} известен рынку через funding (с задержкой до 8ч)
        drift = -planted_strength * sig * np.r_[0, x[:-1]]
    r = 1.2 * rb + idio + drift
    close = 1.5 * np.exp(np.cumsum(r))
    btc = 60000 * np.exp(np.cumsum(rb))
    eth = 3000 * np.exp(np.cumsum(0.9 * rb + 0.5 * sig * rng.standard_normal(n)))

    def ohlc(c, vol_scale):
        o = np.r_[c[0], c[:-1]]
        spread = np.abs(rng.normal(0, 1, n)) * sig * c * 0.6
        h = np.maximum(o, c) + spread
        lo = np.minimum(o, c) - spread * rng.uniform(0.5, 1.0, n)
        v = vol_scale * (1 + 30 * sig) * rng.lognormal(0, 0.4, n)
        return pd.DataFrame({"open": o, "high": h, "low": lo, "close": c, "volume": v}, index=idx)

    perp = ohlc(close, 2e6)
    perp["quote_volume"] = perp.volume * perp.close
    perp["taker_buy_volume"] = perp.volume * np.clip(0.5 + 0.1 * rng.standard_normal(n), 0.05, 0.95)
    perp["symbol"] = np.where(idx < idx[int(n * 0.8)], "SYN_TON", "SYN_GRAM")
    # имитация переименования: 9 дней без торгов
    gap = (idx >= idx[int(n * 0.8)] - pd.Timedelta("9D")) & (idx < idx[int(n * 0.8)])
    perp = perp[~gap].copy()
    perp["gap_before"] = perp.index.to_series().diff().gt(pd.Timedelta("1h")).values
    f_times = idx[idx.hour % 8 == 0]
    fund = pd.Series(1e-4 + 2e-4 * x[idx.hour % 8 == 0] + 2e-5 * rng.standard_normal(len(f_times)),
                     index=f_times, name="funding")
    oi = 5e7 * np.exp(np.cumsum(0.01 * rng.standard_normal(n)))
    m_idx = idx + pd.Timedelta("55min")
    metrics = pd.DataFrame({"oi": oi, "oi_usd": oi * close,
                            "ls_accounts": np.exp(0.2 * rng.standard_normal(n)),
                            "ls_top_positions": np.exp(0.2 * rng.standard_normal(n)),
                            "taker_ls_vol": np.exp(0.1 * rng.standard_normal(n))}, index=m_idx)
    premium = pd.Series(1e-4 * x + 5e-5 * rng.standard_normal(n), index=idx, name="premium")
    panel = {"perp": perp, "funding": fund, "metrics": metrics, "premium": premium.reindex(perp.index),
             "btc": ohlc(btc, 1e4), "eth": ohlc(eth, 1e5), "tf": "1h", "exchange": f"SYNTHETIC-{mode}"}
    from data import quality_check
    panel["quality"] = quality_check(perp, "1h")
    return panel
