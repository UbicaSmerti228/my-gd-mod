"""
hunt.py — «охота за деньгами»: единственная цель — найти способ заработать на GRAM/TON
перпетуале ПОСЛЕ всех издержек. Перебираются самые сильные из известных в крипте
источников доходности, каждый проходит те же проверки, что и в run.py.

Идеи (почему именно они):
  1. donchian4h  — пробой канала на 4h, лонг и/или шорт. Трендследование — самая
                   устойчивая аномалия в крипте; 4h вместо 1h = в разы меньше комиссий.
  2. tsmom4h     — time-series momentum: знак доходности за 7/21 день.
     Обе стратегии могут выбрать режим «только шорт» (side=short_only), если это лучше
     на train: у GRAM структурное давление продаж (анлок ~37 млн GRAM каждые ~30 дней).
  3. pair_*      — относительная стоимость TON/BTC (доллар-нейтрально: TON против BTC).
                   За 90 дней TON отстал от BTC на 42 п.п. — тренд в отношении
                   убирает рыночный риск. Издержки удвоены (две ноги), funding — разность.
  4. carry       — сбор funding: шорт перпетуала + лонг спота, когда funding высокий.
                   Направление цены почти не важно; доход — платежи funding.
  5. unlock      — шорт перед ежемесячным анлоком Believers Fund, закрытие после.
  6. portfolio   — равновзвешенный портфель всех компонентов (веса заданы ЗАРАНЕЕ).
  7. leverage    — сколько даёт плечо 1x…10x и какой ценой (Monte Carlo вероятность
                   разорения), плюс оценка Келли.

Чего здесь НЕТ и почему: мартингейл/усреднение/сетка с плечом (красивый бэктест до
первого тренда, затем ликвидация — на падающем GRAM ровно это и случилось бы);
манипуляции рынком и торговля на инсайде — незаконно.

  python hunt.py                     # Binance, реальные данные
  python hunt.py --synthetic null    # самопроверка без сети
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import warnings

import numpy as np
import pandas as pd

warnings.filterwarnings("ignore")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backtest import BTResult, daily_returns, metrics, run_backtest  # noqa: E402
from config import REPORT_DIR, SEED, CostModel, ResearchConfig, WFConfig, set_seed  # noqa: E402
from data import DataUnavailable, build_panel, quality_check, resample_ohlcv, vision_funding  # noqa: E402
from features import build_features, ema  # noqa: E402
from report import Report, plot_equity  # noqa: E402
from signals import STRATEGIES, _base, _rise  # noqa: E402
from validation import (deflated_sharpe, fold_bounds, monte_carlo, random_entries, sharpe_ci,  # noqa: E402
                        stationary_bootstrap_idx, walk_forward, whites_reality_check)

PREVIOUS_TRIALS = 44   # конфигурации, уже испытанные в run.py — честно добавляем в DSR
# Анлок TON Believers Fund: ~36.6 млн GRAM каждые ~30 дней с октября 2025.
# Якорь подтверждён (KuCoin, 22.09.2026 19:19 UTC); остальные даты = якорь − k·30 дней —
# это ДОПУЩЕНИЕ, сверьте с https://defillama.com/unlocks/ton и при необходимости
# положите точные даты в unlocks.csv (колонка date).
UNLOCK_ANCHOR, UNLOCK_PERIOD_D, UNLOCK_FIRST = "2026-09-22T19:19:00Z", 30, "2025-10-01"


# ------------------------------------------------------------- стратегии 4h
def donchian(f, regime, p):
    s, c = _base(f), f["close_"]
    hi = f["high_"].rolling(p["n"]).max().shift(1)
    lo = f["low_"].rolling(p["n"]).min().shift(1)
    hi_x = f["high_"].rolling(max(p["n"] // 2, 5)).max().shift(1)
    lo_x = f["low_"].rolling(max(p["n"] // 2, 5)).min().shift(1)
    if p["side"] != "short_only":
        s["long_entry"] = _rise(c > hi)
    if p["side"] != "long_only":
        s["short_entry"] = _rise(c < lo)
    s["long_exit"], s["short_exit"] = c < lo_x, c > hi_x          # выход по противоположному полуканалу
    s["stop_atr"], s["max_hold"], s["confluence"] = p["stop"], 10**6, 0.0
    return s


def tsmom(f, regime, p):
    s, c = _base(f), f["close_"]
    mom = np.log(c / c.shift(p["L"]))
    up, dn = mom > 0, mom < 0
    if p["side"] != "short_only":
        s["long_entry"] = _rise(up)
    if p["side"] != "long_only":
        s["short_entry"] = _rise(dn)
    s["long_exit"], s["short_exit"] = dn, up
    s["stop_atr"], s["max_hold"], s["confluence"] = p["stop"], 10**6, 0.0
    return s


HUNT = {
    "donchian4h": (donchian, {"n": [20, 55], "stop": [2.5, 4.0], "side": ["both", "short_only"]}),
    "tsmom4h": (tsmom, {"L": [42, 126], "stop": [3.0, 5.0], "side": ["both", "short_only"]}),
    "pair_donchian": (donchian, {"n": [20, 55], "stop": [2.5, 4.0], "side": ["both"]}),
    "pair_tsmom": (tsmom, {"L": [42, 126], "stop": [3.0, 5.0], "side": ["both"]}),
}
STRATEGIES.update(HUNT)   # регистрируем, чтобы использовать общий walk_forward


def n_configs(name: str) -> int:
    return int(np.prod([len(v) for v in HUNT[name][1].values()]))


# ------------------------------------------------------------ подготовка данных
def to_4h(panel: dict) -> dict:
    p4 = dict(panel)
    for k in ("perp", "btc", "eth"):
        if panel.get(k) is not None and len(panel[k]):
            d = resample_ohlcv(panel[k], "4h")
            p4[k] = d
    perp = p4["perp"]
    perp["gap_before"] = perp.index.to_series().diff().gt(pd.Timedelta("4h")).values
    if panel.get("premium") is not None and len(panel["premium"]):
        p4["premium"] = panel["premium"].resample("4h").last()
    p4["tf"] = "4h"
    return p4


def ratio_panel(p4: dict) -> dict | None:
    """Синтетический инструмент TON/BTC. high/low берутся с запасом (TON_high/BTC_low и
    TON_low/BTC_high), чтобы стопы срабатывали не реже, чем в реальности."""
    t, b = p4["perp"], p4.get("btc")
    if b is None or not len(b):
        return None
    b = b.reindex(t.index)
    ok = b["close"].notna()
    t, b = t[ok], b[ok]
    r = pd.DataFrame({"open": t.open / b.open, "close": t.close / b.close,
                      "high": t.high / b.low, "low": t.low / b.high, "volume": t.volume}, index=t.index)
    r["high"] = r[["open", "close", "high"]].max(axis=1)
    r["low"] = r[["open", "close", "low"]].min(axis=1)
    r["gap_before"] = t["gap_before"].values
    r["symbol"] = "TON/BTC"
    return {"perp": r, "btc": None, "eth": None, "funding": None, "metrics": None, "premium": None, "tf": "4h"}


def net_pair_funding(ton_f: pd.Series, btc_f: pd.Series | None) -> pd.Series:
    """Лонг TON/BTC = лонг TON + шорт BTC: платим funding TON, получаем funding BTC."""
    if btc_f is None or not len(btc_f):
        return ton_f
    idx = ton_f.index.union(btc_f.index)
    return (ton_f.reindex(idx).fillna(0) - btc_f.reindex(idx).fillna(0)).rename("funding")


# ------------------------------------------------------------------ carry
CARRY_GRID = [{"thr_ann": t, "k": k} for t in (0.05, 0.10, 0.20) for k in (3, 9)]


def carry_returns(perp: pd.DataFrame, funding: pd.Series, premium: pd.Series, p: dict,
                  spot_fee: float = 0.001, costs: CostModel | None = None, perp_lev: float = 3.0,
                  mmr: float = 0.01) -> tuple[pd.Series, int]:
    """Часовые доходности капитала для «шорт перп + лонг спот».
    Номинал N = C/(1+1/L): спот оплачен полностью, перп — маржой N/L.
    PnL = funding (шорт получает положительный) − изменение премии перпа к индексу − комиссии.
    Ликвидация шорт-перпа при росте цены > 1/L − mmr от входа (спот-нога её компенсирует,
    но позиция закрывается с потерей ~mmr и комиссий). Вход/выход — по прошлому funding (каузально)."""
    costs = costs or CostModel()
    idx = perp.index
    f = funding.sort_index()
    per_year = 365 * 24 / max(f.index.to_series().diff().median() / pd.Timedelta("1h"), 1)
    sig = (f.rolling(p["k"]).mean() * per_year).rename("ann")
    close_t = idx + pd.Timedelta("1h")
    right = pd.DataFrame({"t": sig.index.astype("datetime64[ns, UTC]"), "ann": sig.to_numpy()})
    left = pd.DataFrame({"t": close_t.astype("datetime64[ns, UTC]")})
    ann = pd.merge_asof(left, right, on="t", direction="backward")["ann"].to_numpy()
    from backtest import funding_per_bar
    fb = funding_per_bar(idx, "1h", f)
    prem = premium.reindex(idx).ffill().fillna(0).to_numpy() if premium is not None and len(premium) else np.zeros(len(idx))
    c = perp["close"].to_numpy()
    gap_next = np.r_[perp["gap_before"].to_numpy(bool)[1:], False]
    frac = 1 / (1 + 1 / perp_lev)
    rt_cost = frac * (spot_fee + costs.taker_fee + 2 * costs.slippage_bps * 1e-4)  # одна сторона, обе ноги
    r = np.zeros(len(idx))
    on, entry_px, n_trades = False, 0.0, 0
    for i in range(1, len(idx)):
        if on:
            r[i] += frac * fb[i]                              # шорт получает положительный funding
            r[i] -= frac * (prem[i] - prem[i - 1])            # расширение премии — убыток шорта перпа
            if c[i] / entry_px - 1 > 1 / perp_lev - mmr:      # ликвидация перп-ноги
                r[i] -= frac * mmr + rt_cost
                on = False
                continue
            if ann[i] < 0 or gap_next[i]:
                r[i] -= rt_cost
                on = False
        elif np.isfinite(ann[i]) and ann[i] > p["thr_ann"] and not gap_next[i]:
            r[i] -= rt_cost
            on, entry_px = True, c[i]
            n_trades += 1
    return pd.Series(r, idx), n_trades


def carry_walk_forward(perp, funding, premium, cfg: ResearchConfig) -> dict:
    wf = cfg.wf
    out, rows, cfg_rets = [], [], {j: [] for j in range(len(CARRY_GRID))}
    full = {j: carry_returns(perp, funding, premium, p, costs=cfg.costs) for j, p in enumerate(CARRY_GRID)}
    for k, (a, b, c) in enumerate(fold_bounds(len(perp), wf.train_bars, wf.test_bars, wf.step_bars)):
        sh = []
        for j in range(len(CARRY_GRID)):
            rr = full[j][0].iloc[a:b]
            d = (1 + rr).cumprod().resample("1D").last().pct_change().dropna()
            sh.append(d.mean() / d.std() * np.sqrt(365) if d.std() > 0 else np.nan)
            cfg_rets[j].append((1 + full[j][0].iloc[b:c]).cumprod().resample("1D").last().pct_change().dropna())
        sh = np.array(sh, float)
        best = int(np.nanargmax(sh)) if np.isfinite(sh).any() else None
        use = best is not None and sh[best] > 0
        seg = full[best][0].iloc[b:c] if use else pd.Series(0.0, perp.index[b:c])
        out.append(seg)
        rows.append({"fold": k, "test_start": perp.index[b], "params": CARRY_GRID[best] if use else None,
                     "train_sharpe": sh[best] if best is not None else np.nan})
    r = pd.concat(out)
    eq = cfg.risk.initial_capital * (1 + r).cumprod()
    cr = pd.DataFrame({f"carry|{CARRY_GRID[j]}": pd.concat(v) for j, v in cfg_rets.items()})
    n_tr = sum(full[j][1] for j in range(len(CARRY_GRID)))
    return {"equity": eq, "folds": pd.DataFrame(rows), "config_returns": cr, "entries_all_configs": n_tr}


# ------------------------------------------------------------------ unlock
def unlock_dates(end: pd.Timestamp) -> list[pd.Timestamp]:
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unlocks.csv")
    if os.path.exists(path):
        return [pd.Timestamp(d, tz="UTC") for d in pd.read_csv(path)["date"]]
    a, out = pd.Timestamp(UNLOCK_ANCHOR), []
    t = a
    while t >= pd.Timestamp(UNLOCK_FIRST, tz="UTC"):
        out.append(t)
        t -= pd.Timedelta(days=UNLOCK_PERIOD_D)
    t = a + pd.Timedelta(days=UNLOCK_PERIOD_D)
    while t <= end:
        out.append(t)
        t += pd.Timedelta(days=UNLOCK_PERIOD_D)
    return sorted(out)


def unlock_permutation(perp: pd.DataFrame, us: pd.DataFrame, cfg: ResearchConfig, n: int = 2000,
                       seed: int = SEED) -> pd.DataFrame:
    """Контроль тренда: тот же шорт, но в СЛУЧАЙНЫЕ даты того же периода. Если GRAM просто
    падал, шорт «под анлок» выглядит прибыльным без всякого эффекта анлока.
    excess = среднее по анлокам − среднее по случайным датам; p = доля случайных ≥ наблюдаемого."""
    rng = np.random.default_rng(seed)
    o = perp["open"]
    cost = 2 * (cfg.costs.taker_fee + cfg.costs.slippage_bps * 1e-4)
    t0, t1 = pd.Timestamp(us["unlock"].min(), tz="UTC"), pd.Timestamp(us["unlock"].max(), tz="UTC")
    span = (t1 - t0).days + 1
    rows = {}
    for (d, a), g in us.groupby(["d_before", "a_after"]):
        k, obs, sims = len(g), g["net"].mean(), []
        for _ in range(n):
            vals = []
            for T in t0 + pd.to_timedelta(rng.integers(0, span, k), unit="D"):
                i0 = o.index.searchsorted(T - pd.Timedelta(days=d))
                i1 = o.index.searchsorted(T + pd.Timedelta(days=a))
                if 0 < i0 < i1 < len(o):
                    vals.append(-(o.iloc[i1] / o.iloc[i0] - 1) - cost)
            sims.append(np.mean(vals) if vals else np.nan)
        sims = np.array(sims, float)
        rows[(d, a)] = {"events": k, "mean_net_unlock": obs, "mean_net_random_dates": np.nanmean(sims),
                        "excess": obs - np.nanmean(sims), "p_perm": float(np.nanmean(sims >= obs))}
    out = pd.DataFrame(rows).T
    out["p_bonferroni"] = (out["p_perm"] * len(out)).clip(upper=1.0)   # 6 окон проверены одновременно
    return out


def unlock_study(perp: pd.DataFrame, funding: pd.Series, cfg: ResearchConfig) -> pd.DataFrame:
    """Шорт с open первого бара ≥ T−d до open первого бара ≥ T+a. Все 6 вариантов показаны
    (без выбора лучшего), издержки: 2×(taker+slippage) + funding."""
    rows = []
    cost = 2 * (cfg.costs.taker_fee + cfg.costs.slippage_bps * 1e-4)
    o = perp["open"]
    for T in unlock_dates(perp.index[-1]):
        for d in (1, 3, 7):
            for a in (1, 3):
                i0 = o.index.searchsorted(T - pd.Timedelta(days=d))
                i1 = o.index.searchsorted(T + pd.Timedelta(days=a))
                if i0 <= 0 or i1 >= len(o) or perp["gap_before"].iloc[i0 + 1:i1 + 1].any():
                    continue
                gross = -(o.iloc[i1] / o.iloc[i0] - 1)
                fsum = funding.loc[o.index[i0]:o.index[i1]].sum() if funding is not None and len(funding) else 0.0
                rows.append({"unlock": T.date(), "d_before": d, "a_after": a, "gross": gross,
                             "funding": fsum, "net": gross + fsum - cost})
    return pd.DataFrame(rows)


# ------------------------------------------------------------ плечо и Келли
def leverage_table(daily: pd.Series, levels=(1, 2, 3, 5, 10), horizon: int = 365, n: int = 3000,
                   block: float = 10, seed: int = SEED) -> pd.DataFrame:
    """Масштабирование дневных доходностей на k (упрощение: внутридневные ликвидации не
    моделируются, поэтому реальный риск ВЫШЕ). Разорение = капитал упал до 10% от старта."""
    r = daily.dropna().to_numpy()
    if len(r) < 60:
        return pd.DataFrame()
    rng = np.random.default_rng(seed)
    paths = [r[stationary_bootstrap_idx(len(r), block, rng)][:horizon] if len(r) >= horizon
             else r[stationary_bootstrap_idx(horizon, block, rng) % len(r)] for _ in range(n)]
    rows = {}
    for k in levels:
        fin, dd50, ruin = [], 0, 0
        for x in paths:
            eq = np.cumprod(np.maximum(1 + k * x, 0.0))
            fin.append(eq[-1] - 1)
            peak = np.maximum.accumulate(eq)
            dd50 += (eq / peak - 1).min() <= -0.5
            ruin += eq.min() <= 0.1
        fin = np.array(fin)
        rows[f"{k}x"] = {"median_1y": np.median(fin), "p05_1y": np.quantile(fin, .05),
                         "p95_1y": np.quantile(fin, .95), "P(убыток)": (fin < 0).mean(),
                         "P(просадка>50%)": dd50 / n, "P(разорение)": ruin / n}
    return pd.DataFrame(rows).T


def kelly(daily: pd.Series, n: int = 2000, seed: int = SEED) -> dict:
    r = daily.dropna().to_numpy()
    if len(r) < 60 or r.var() == 0:
        return {}
    rng = np.random.default_rng(seed)
    bs = [(x.mean() / x.var()) for x in (r[stationary_bootstrap_idx(len(r), 10, rng)] for _ in range(n))]
    return {"kelly_full": float(r.mean() / r.var()), "kelly_p05": float(np.quantile(bs, .05)),
            "kelly_p95": float(np.quantile(bs, .95))}


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--exchange", default="binance")
    ap.add_argument("--since", default="2022-01-01")
    ap.add_argument("--synthetic", choices=["null", "planted"], default=None)
    ap.add_argument("--fast", action="store_true")
    args = ap.parse_args(argv)
    set_seed()
    t0 = time.time()

    if args.synthetic:
        from synthetic import make_panel
        panel, tag = make_panel(mode=args.synthetic), f"synthetic_{args.synthetic}"
        btc_f = None
    else:
        try:
            panel = build_panel(args.exchange, "1h", args.since)
        except DataUnavailable as e:
            print(f"[ОСТАНОВ] Данные недоступны: {e}")
            return 2
        tag = args.exchange
        try:
            btc_f = vision_funding("BTCUSDT", panel["perp"].index[0], pd.Timestamp.now(tz="UTC"))
        except Exception as e:  # noqa: BLE001
            print(f"[hunt] funding BTC недоступен ({e}); пара считается без него")
            btc_f = None

    cfg = ResearchConfig(exchange=args.exchange, base_tf="4h")
    cfg.wf = WFConfig(train_bars=6 * 180, test_bars=6 * 60, step_bars=6 * 60, min_trades_train=8)
    if args.fast:
        cfg.mc_sims, cfg.random_entry_sims = 300, 40
    cfg_pair = ResearchConfig(exchange=args.exchange, base_tf="4h", wf=cfg.wf)
    cfg_pair.costs = CostModel(taker_fee=2 * cfg.costs.taker_fee, slippage_bps=2 * cfg.costs.slippage_bps)
    cfg1h = ResearchConfig(exchange=args.exchange, base_tf="1h")

    out_dir = os.path.join(REPORT_DIR, f"hunt_{tag}")
    rep = Report(out_dir, f"Охота за доходностью: GRAM/TON perp ({tag})")
    if args.synthetic:
        rep.p("> **СИНТЕТИКА — проверка кода, не рынок.**")

    p4 = to_4h(panel)
    df4 = p4["perp"]
    f4 = build_features(p4, "4h").assign(high_=df4["high"], low_=df4["low"])
    rp = ratio_panel(p4)
    comps, all_cfg = {}, []
    funding = panel["funding"]

    print("[hunt] 4h стратегии")
    for name in ("donchian4h", "tsmom4h"):
        comps[name] = ("wf", walk_forward(name, f4, df4, funding, cfg, regime_method="rule"), f4, df4, cfg)
        all_cfg.append(comps[name][1].config_oos_returns)
    if rp is not None:
        dfr = rp["perp"]
        fr = build_features(rp, "4h").assign(high_=dfr["high"], low_=dfr["low"])
        nf = net_pair_funding(funding, btc_f)
        for name in ("pair_donchian", "pair_tsmom"):
            comps[name] = ("wf", walk_forward(name, fr, dfr, nf, cfg_pair, regime_method="rule"), fr, dfr, cfg_pair)
            all_cfg.append(comps[name][1].config_oos_returns)

    print("[hunt] carry")
    carry = None
    if len(funding):
        carry = carry_walk_forward(panel["perp"], funding, panel.get("premium"), cfg1h)
        all_cfg.append(carry["config_returns"])

    R_all = pd.concat(all_cfg, axis=1)
    trial_sr = (R_all.mean() / R_all.std()).to_numpy()
    n_trials = PREVIOUS_TRIALS + R_all.shape[1] + 6   # +6 вариантов анлок-стратегии
    rc = whites_reality_check(R_all, None, 500 if args.fast else 2000)

    # ------------------------------------------------------- таблица компонентов
    rows, daily, curves, checks = {}, {}, {}, {}
    for name, (kind, wf, f_, df_, c_) in comps.items():
        res = wf.oos
        m = metrics(res)
        r = daily_returns(res.equity)
        daily[name], curves[name] = r, res.equity
        dsr = deflated_sharpe(r, trial_sr, n_trials)
        ci = sharpe_ci(r, 500 if args.fast else 2000)
        mc = monte_carlo(res, cfg.mc_sims)
        rnd = random_entries(df_.loc[res.equity.index[0]:res.equity.index[-1]],
                             f_.loc[res.equity.index[0]:res.equity.index[-1]], None, c_, res,
                             cfg.random_entry_sims) if m["Trades"] >= 5 else {}
        rows[name] = {**{k: m.get(k) for k in ("CAGR", "Sharpe", "MaxDD", "ProfitFactor", "WinRate", "Trades",
                                                 "TimeInMarket", "Fees$", "Funding$")},
                      "Sharpe_CI_lo": ci.get("ci05"), "DSR": dsr.get("DSR"), "DSR(var=1/T)": dsr.get("DSR_nullvar"),
                      "Random_p": rnd.get("p_value(random>=strategy)"), "MC_P(loss)": mc.get("P(total_ret<0)")}
        checks[name] = [m.get("Trades", 0) >= 30, (m.get("Sharpe") or -9) > 0.5, (dsr.get("DSR") or 0) > 0.95,
                        rc.get("p_value", 1) < 0.05, rnd.get("p_value(random>=strategy)", 1) < 0.05,
                        mc.get("P(total_ret<0)", 1) < 0.2]
    if carry is not None:
        r = daily_returns(carry["equity"])
        daily["carry"], curves["carry"] = r, carry["equity"]
        dd = (carry["equity"] / carry["equity"].cummax() - 1).min()
        yrs = (r.index[-1] - r.index[0]).days / 365.25
        cagr = (carry["equity"].iloc[-1] / carry["equity"].iloc[0]) ** (1 / max(yrs, 1e-9)) - 1
        dsr = deflated_sharpe(r, trial_sr, n_trials)
        ci = sharpe_ci(r, 500 if args.fast else 2000)
        rows["carry"] = {"CAGR": cagr, "Sharpe": ci.get("sharpe"), "MaxDD": dd, "Trades": carry["entries_all_configs"],
                         "Sharpe_CI_lo": ci.get("ci05"), "DSR": dsr.get("DSR"), "DSR(var=1/T)": dsr.get("DSR_nullvar")}
        checks["carry"] = [True, (ci.get("sharpe") or -9) > 0.5, (dsr.get("DSR") or 0) > 0.95,
                           rc.get("p_value", 1) < 0.05, True, (ci.get("P(sharpe<=0)") or 1) < 0.2]

    rep.h("1. Компоненты (walk-forward, только вне обучения, после всех издержек)")
    rep.p(f"Всего испытаний за проект (для DSR): **{n_trials}** (включая {PREVIOUS_TRIALS} из run.py). "
          f"White's Reality Check по всем {R_all.shape[1]} конфигурациям этого прогона: p = {rc.get('p_value', np.nan):.3f}.")
    tab = pd.DataFrame(rows).T
    rep.table(tab)
    names = ["≥30 сделок", "Sharpe>0.5", "DSR>0.95", "RC p<0.05", "лучше случайных", "MC P(убыток)<20%"]
    passed = {k: all(v) for k, v in checks.items()}
    for k, v in checks.items():
        rep.p(f"- **{k}: {'EDGE ✔' if passed[k] else 'edge не доказан'}** — " +
              "; ".join(f"{'✔' if ok else '✘'} {n}" for n, ok in zip(names, v)))
    for name, (kind, wf, *_rest) in comps.items():
        fd = wf.folds.copy(); fd["params"] = fd["params"].astype(str)
        rep.p(f"<details><summary>{name}: фолды</summary>\n")
        rep.table(fd.set_index("fold")[["test_start", "params", "train_sharpe_robust", "test_sharpe", "test_trades"]])
        rep.p("</details>")
    if carry is not None:
        rep.p("**carry: фолды** (выбор порога funding на train)")
        cf = carry["folds"].copy(); cf["params"] = cf["params"].astype(str)
        rep.table(cf.set_index("fold"))

    # ------------------------------------------------------------ портфель
    rep.h("2. Портфель (равные веса, заданы заранее)")
    D = pd.DataFrame(daily).dropna(how="all").fillna(0.0)
    port = D.mean(axis=1)
    peq = cfg.risk.initial_capital * (1 + port).cumprod()
    curves["PORTFOLIO"] = peq
    ci = sharpe_ci(port, 500 if args.fast else 2000)
    dsr = deflated_sharpe(port, trial_sr, n_trials)
    yrs = (port.index[-1] - port.index[0]).days / 365.25
    pm = {"CAGR": (peq.iloc[-1] / peq.iloc[0]) ** (1 / max(yrs, 1e-9)) - 1, "Sharpe": ci.get("sharpe"),
          "Sharpe_CI90": f"[{ci.get('ci05', np.nan):.2f}; {ci.get('ci95', np.nan):.2f}]",
          "MaxDD": (peq / peq.cummax() - 1).min(), "DSR": dsr.get("DSR"), "DSR(var=1/T)": dsr.get("DSR_nullvar"),
          "P(Sharpe<=0)": ci.get("P(sharpe<=0)")}
    rep.table(pm)
    rep.p("Корреляции компонентов (дневные доходности):")
    rep.table(D.corr())
    rep.img(plot_equity(curves, rep.path("hunt_equity.png"), "Капитал вне обучения: компоненты и портфель"), "eq")

    # ------------------------------------------------------------ плечо
    rep.h("3. Плечо: сколько можно заработать и какой ценой")
    best = max(daily, key=lambda k: (daily[k].mean() / daily[k].std()) if daily[k].std() > 0 else -9)
    for nm, series in (("PORTFOLIO", port), (best, daily[best])):
        rep.p(f"**{nm}** — Monte Carlo на 1 год (stationary bootstrap дневных доходностей вне обучения):")
        rep.table(leverage_table(series))
        if nm == "carry":
            rep.p("⚠️ Для carry таблица плеча НЕ годится: плечо здесь — это плечо шорт-ноги перпетуала, "
                  "и при 10x рост цены на ~9% ликвидирует её (в мае 2026 GRAM вырос на 120% за неделю). "
                  "Разумный предел — 2–3x с запасом маржи.")
            continue
        k = kelly(series)
        if k:
            rep.p(f"Келли (доля капитала × плечо): полный {k['kelly_full']:.2f}, 90% CI [{k['kelly_p05']:.2f}; "
                  f"{k['kelly_p95']:.2f}]. Если нижняя граница ≤ 0 — оптимальное плечо статистически "
                  "неотличимо от нуля, т.е. любое плечо — ставка на удачу.")

    # ------------------------------------------------------------ анлоки
    rep.h("4. Шорт под анлок Believers Fund")
    us = unlock_study(panel["perp"], funding, cfg1h)
    if len(us):
        g = us.groupby(["d_before", "a_after"])["net"]
        summ = pd.DataFrame({"events": g.size(), "mean_net": g.mean(), "hit_rate": g.apply(lambda x: (x > 0).mean()),
                             "t": g.mean() / (g.std() / np.sqrt(g.size()))})
        rep.p("Даты анлоков — допущение (якорь 22.09.2026 ± 30 дней), см. UNLOCK_* в hunt.py. "
              "Показаны ВСЕ 6 вариантов окна, без выбора лучшего:")
        rep.table(summ)
        rep.p("Контроль тренда — тот же шорт в случайные даты того же периода (без funding). "
              "Эффект анлока есть, только если excess > 0 и p_bonferroni < 0.05 (поправка на 6 окон; "
              "на случайных данных без поправки ложные «находки» появляются регулярно):")
        rep.table(unlock_permutation(panel["perp"], us, cfg1h, 300 if args.fast else 2000))
        rep.p("<details><summary>по событиям</summary>\n")
        rep.table(us.set_index("unlock"))
        rep.p("</details>")
    else:
        rep.p("Нет событий в периоде данных.")

    # ------------------------------------------------------------ сейчас
    rep.h("5. Что система делает прямо сейчас")
    from run import current_signal
    now = {}
    for name, (kind, wf, f_, df_, c_) in comps.items():
        try:
            cs = current_signal(name, wf, f_, df_, c_)
            now[name] = {"вход сейчас": cs.get("new_entry_signal"), "в позиции": cs.get("strategy_in_position_now"),
                         "позиция": json.dumps(cs.get("open_position", ""), default=str),
                         "стоп": cs.get("stop"), "параметры": json.dumps(cs.get("params"), default=str),
                         "edge доказан": passed.get(name)}
        except Exception as e:  # noqa: BLE001
            now[name] = {"ошибка": str(e)}
    if carry is not None and len(funding):
        last = carry["folds"].dropna(subset=["params"])
        if len(last):
            p = last.iloc[-1]["params"]
            f = funding.sort_index()
            per_year = 365 * 24 / max(f.index.to_series().diff().median() / pd.Timedelta("1h"), 1)
            ann = float(f.iloc[-p["k"]:].mean() * per_year)
            now["carry"] = {"вход сейчас": "ДА (шорт перп + лонг спот)" if ann > p["thr_ann"] else "нет",
                            "в позиции": ann > 0, "позиция": f"funding (ann) = {ann:.1%}, порог {p['thr_ann']:.0%}",
                            "edge доказан": passed.get("carry")}
    rep.table(pd.DataFrame(now).T)
    winners = [k for k, v in passed.items() if v]
    rep.h("6. Вердикт")
    if winners:
        rep.p(f"Прошли ВСЕ проверки: **{', '.join(winners)}**. Это кандидаты на бумажную торговлю "
              "(2–3 месяца) перед реальными деньгами.")
    else:
        rep.p("**Ни один способ не прошёл все проверки.** Деньги на этих идеях заработать можно только "
              "случайно; плечо в такой ситуации ускоряет потерю, а не заработок.")
    p = rep.save()
    with open(os.path.join(out_dir, "summary.json"), "w") as fh:
        json.dump({"components": rows, "portfolio": pm, "passed": passed, "now": now, "n_trials": n_trials},
                  fh, default=str, indent=1)
    print(f"\nГотово за {time.time() - t0:.0f} c. Отчёт: {p}")
    for k, v in passed.items():
        print(f"  {k:14s}: {'EDGE' if v else 'edge не доказан'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
