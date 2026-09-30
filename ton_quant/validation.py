"""
validation.py — проверка устойчивости и статистической значимости.

  * walk_forward      — скользящие окна train/test, выбор параметров ТОЛЬКО на train
                        по «робастному» критерию (медиана Sharpe по соседям в сетке);
  * purged_kfold      — purged/embargoed K-fold (López de Prado) для ML;
  * Monte Carlo       — перестановка и bootstrap сделок, stationary bootstrap Sharpe;
  * deflated_sharpe   — Deflated Sharpe Ratio (Bailey & López de Prado, 2014);
  * whites_rc         — White's Reality Check (stationary bootstrap, Politis-Romano);
  * random_entries    — бенчмарк случайных входов с теми же издержками.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd
from scipy.stats import kurtosis, norm, skew

from backtest import BTResult, daily_returns, metrics, run_backtest
from config import SEED, ResearchConfig
from regime import fit_predict_regime
from signals import generate, param_grid

EULER_GAMMA = 0.5772156649
_REGIME_CACHE: dict = {}


# ---------------------------------------------------------------- walk-forward
def fold_bounds(n: int, train: int, test: int, step: int) -> list[tuple[int, int, int]]:
    out, s = [], 0
    while s + train + test <= n:
        out.append((s, s + train, s + train + test))
        s += step
    if not out and n > train:            # короткая история: один фолд с остатком
        out.append((0, train, n))
    return out


def _neighbors(grid: list[dict], i: int) -> list[int]:
    """Соседи в сетке: отличаются ровно одним параметром на один шаг."""
    keys = list(grid[0])
    levels = {k: sorted({g[k] for g in grid}) for k in keys}
    me = grid[i]
    res = [i]
    for j, g in enumerate(grid):
        diff = [k for k in keys if g[k] != me[k]]
        if len(diff) == 1:
            k = diff[0]
            if abs(levels[k].index(g[k]) - levels[k].index(me[k])) == 1:
                res.append(j)
    return res


@dataclass
class WFResult:
    strategy: str
    oos: BTResult                    # склеенный OOS
    folds: pd.DataFrame              # по фолду: параметры, Sharpe train/test, сделки
    grid_scores: pd.DataFrame        # Sharpe на train для каждой конфигурации в каждом фолде
    config_oos_returns: pd.DataFrame  # дневные OOS-доходности КАЖДОЙ фикс. конфигурации (для RC/DSR)
    n_trials: int


def _chain(results: list[BTResult], capital: float) -> BTResult:
    """Склейка OOS-кусков: доходности каждого куска применяются к текущему капиталу."""
    eqs, trs, pos = [], [], []
    level = capital
    for r in results:
        rel = r.equity / r.equity.iloc[0]
        e = rel * level
        level = e.iloc[-1]
        eqs.append(e); pos.append(r.position)
        if len(r.trades):
            trs.append(r.trades)
    costs = {}
    for r in results:
        for k, v in r.costs.items():
            costs[k] = costs.get(k, 0) + v
    return BTResult(pd.concat(eqs), pd.concat(trs, ignore_index=True) if trs else pd.DataFrame(),
                    pd.concat(pos), costs)


def walk_forward(name: str, f: pd.DataFrame, df: pd.DataFrame, funding: pd.Series | None,
                 cfg: ResearchConfig, regime_method: str = "hmm", verbose: bool = True) -> WFResult:
    grid = param_grid(name)
    wf = cfg.wf
    bounds = fold_bounds(len(df), wf.train_bars, wf.test_bars, wf.step_bars)
    rows, scores, oos_parts = [], [], []
    cfg_rets = {j: [] for j in range(len(grid))}
    for k, (a, b, c) in enumerate(bounds):
        tr_idx, te_idx = df.index[a:b], df.index[b:c]
        key = (id(f), a, b, c, regime_method)
        if key not in _REGIME_CACHE:                                      # фит только на train
            _REGIME_CACHE[key] = fit_predict_regime(f.iloc[:c], tr_idx, regime_method)
        regime = _REGIME_CACHE[key]
        sh = np.full(len(grid), np.nan)
        test_res = {}
        for j, p in enumerate(grid):
            sig = generate(name, f.iloc[:c], df["close"].iloc[:c], regime, p)
            r_tr = run_backtest(df.iloc[a:b], sig.iloc[a:b], f["atr"], cfg.base_tf, funding,
                                cfg.costs, cfg.risk, regime)
            m = metrics(r_tr)
            sh[j] = m["Sharpe"] if m["Trades"] >= wf.min_trades_train else np.nan
            # OOS каждой конфигурации — только для поправки на множественное тестирование
            r_te = run_backtest(df.iloc[b:c], sig.iloc[b:c], f["atr"], cfg.base_tf, funding,
                                cfg.costs, cfg.risk, regime)
            test_res[j] = r_te
            cfg_rets[j].append(daily_returns(r_te.equity))
        robust = np.array([np.nanmedian(sh[_neighbors(grid, j)]) if np.isfinite(sh[j]) else np.nan
                           for j in range(len(grid))])
        if np.all(~np.isfinite(robust)):
            best = None
        else:
            best = int(np.nanargmax(robust))
        scores.append(pd.Series(sh, name=k))
        if best is None or robust[best] <= 0:
            # на train нет конфигурации с положительным робастным Sharpe -> вне рынка
            flat = BTResult(pd.Series(cfg.risk.initial_capital, te_idx), pd.DataFrame(),
                            pd.Series(0.0, te_idx), {})
            oos_parts.append(flat)
            rows.append({"fold": k, "train_start": tr_idx[0], "test_start": te_idx[0], "test_end": te_idx[-1],
                         "params": None, "train_sharpe_robust": np.nanmax(robust) if np.isfinite(robust).any() else np.nan,
                         "test_sharpe": np.nan, "test_trades": 0, "note": "flat (no robust edge in train)"})
        else:
            r = test_res[best]
            oos_parts.append(r)
            mt = metrics(r)
            rows.append({"fold": k, "train_start": tr_idx[0], "test_start": te_idx[0], "test_end": te_idx[-1],
                         "params": grid[best], "train_sharpe_robust": robust[best],
                         "test_sharpe": mt["Sharpe"], "test_trades": mt["Trades"], "note": ""})
        if verbose:
            print(f"  [{name}] fold {k + 1}/{len(bounds)}: {rows[-1]['params']} "
                  f"train={rows[-1]['train_sharpe_robust']:.2f} test={rows[-1]['test_sharpe']:.2f}")
    oos = _chain(oos_parts, cfg.risk.initial_capital)
    cr = pd.DataFrame({j: pd.concat(v) for j, v in cfg_rets.items()})
    cr.columns = [f"{name}|{grid[j]}" for j in range(len(grid))]
    return WFResult(name, oos, pd.DataFrame(rows), pd.DataFrame(scores), cr, len(grid))


def sensitivity_summary(wf: WFResult) -> pd.DataFrame:
    """Карта устойчивости: по каждой конфигурации — медианный Sharpe на train по фолдам,
    доля фолдов с Sharpe>0. Устойчивая стратегия = широкое «плато», а не один пик."""
    grid = param_grid(wf.strategy)
    s = wf.grid_scores
    df = pd.DataFrame(grid)
    df["median_train_sharpe"] = s.median(axis=0).values
    df["share_folds_positive"] = (s > 0).mean(axis=0).values
    df["median_oos_sharpe"] = [
        _sharpe(wf.config_oos_returns.iloc[:, j].dropna()) for j in range(len(grid))]
    return df


def _sharpe(r: pd.Series, ann: int = 365) -> float:
    return float(r.mean() / r.std() * np.sqrt(ann)) if len(r) > 2 and r.std() > 0 else np.nan


# --------------------------------------------------------- purged / embargo CV
def purged_kfold(times: pd.DatetimeIndex, label_end: pd.DatetimeIndex, k: int = 5,
                 embargo_frac: float = 0.01):
    """Генератор (train_idx, test_idx). Из train удаляются наблюдения, чьё окно метки
    [t, label_end] пересекается с тестом, плюс эмбарго после теста."""
    n = len(times)
    folds = np.array_split(np.arange(n), k)
    emb = int(n * embargo_frac)
    le = np.asarray(label_end.values)
    t = np.asarray(times.values)
    for test in folds:
        t0, t1 = t[test[0]], le[test].max()
        train = np.arange(n)
        overlap = (t[train] <= t1) & (le[train] >= t0)
        after = (train > test[-1]) & (train <= test[-1] + emb)
        yield train[~overlap & ~after], test


# ------------------------------------------------------------------ Monte Carlo
def trade_returns(res: BTResult) -> np.ndarray:
    """Доходность каждой сделки относительно капитала на момент входа."""
    t = res.trades
    if not len(t):
        return np.array([])
    eq_at_entry = res.equity.reindex(pd.to_datetime(t.entry_time), method="ffill").to_numpy()
    eq_at_entry = np.where(eq_at_entry > 0, eq_at_entry, np.nan)
    return (t.net.to_numpy() / eq_at_entry)


def _max_dd(path: np.ndarray) -> float:
    return float((path / np.maximum.accumulate(path) - 1).min())


def monte_carlo(res: BTResult, n_sims: int = 2000, seed: int = SEED) -> dict:
    tr = trade_returns(res)
    tr = tr[np.isfinite(tr)]
    if len(tr) < 5:
        return {"n_trades": len(tr), "note": "слишком мало сделок для Monte Carlo"}
    rng = np.random.default_rng(seed)
    shuf_dd, boot_ret, boot_dd = [], [], []
    for _ in range(n_sims):
        p = rng.permutation(tr)
        shuf_dd.append(_max_dd(np.cumprod(1 + p)))
        b = rng.choice(tr, size=len(tr), replace=True)
        path = np.cumprod(1 + b)
        boot_ret.append(path[-1] - 1)
        boot_dd.append(_max_dd(path))
    q = lambda x, a: float(np.quantile(x, a))  # noqa: E731
    return {"n_trades": len(tr),
            "shuffle_maxdd_p50": q(shuf_dd, .5), "shuffle_maxdd_p05": q(shuf_dd, .05),
            "boot_total_ret_p05": q(boot_ret, .05), "boot_total_ret_p50": q(boot_ret, .5),
            "boot_total_ret_p95": q(boot_ret, .95), "boot_maxdd_p05": q(boot_dd, .05),
            "P(total_ret<0)": float(np.mean(np.array(boot_ret) < 0))}


def stationary_bootstrap_idx(T: int, block: float, rng) -> np.ndarray:
    """Индексы stationary bootstrap (Politis & Romano, 1994), средняя длина блока = block."""
    idx = np.empty(T, dtype=int)
    idx[0] = rng.integers(T)
    p = 1.0 / block
    for t in range(1, T):
        idx[t] = rng.integers(T) if rng.random() < p else (idx[t - 1] + 1) % T
    return idx


def sharpe_ci(r: pd.Series, n_boot: int = 2000, block: float = 10, seed: int = SEED, ann: int = 365) -> dict:
    r = r.dropna().to_numpy()
    if len(r) < 30 or r.std() == 0:
        return {"sharpe": np.nan, "ci05": np.nan, "ci95": np.nan}
    rng = np.random.default_rng(seed)
    bs = []
    for _ in range(n_boot):
        x = r[stationary_bootstrap_idx(len(r), block, rng)]
        bs.append(x.mean() / x.std() * np.sqrt(ann) if x.std() > 0 else 0.0)
    return {"sharpe": float(r.mean() / r.std() * np.sqrt(ann)),
            "ci05": float(np.quantile(bs, .05)), "ci95": float(np.quantile(bs, .95)),
            "P(sharpe<=0)": float(np.mean(np.array(bs) <= 0))}


# ------------------------------------------------- множественное тестирование
def probabilistic_sharpe(sr: float, sr_bench: float, T: int, sk: float, ku: float) -> float:
    """PSR: P(SR_true > sr_bench). sr — НЕ аннуализированный (по периодам), ku — обычный эксцесс+3."""
    den = np.sqrt(max(1 - sk * sr + (ku - 1) / 4 * sr ** 2, 1e-12))
    return float(norm.cdf((sr - sr_bench) * np.sqrt(T - 1) / den))


def deflated_sharpe(returns: pd.Series, trial_sharpes: np.ndarray) -> dict:
    """DSR: PSR против порога SR0, ожидаемого максимума Sharpe среди N испытаний без edge."""
    r = returns.dropna().to_numpy()
    T = len(r)
    tsr = np.asarray(trial_sharpes, float)
    tsr = tsr[np.isfinite(tsr)]
    N = max(len(tsr), 1)
    if T < 30 or r.std() == 0:
        return {"DSR": np.nan, "note": "мало наблюдений"}
    sr = r.mean() / r.std()
    var_sr = np.var(tsr, ddof=1) if len(tsr) > 1 else 1.0 / T
    sr0 = np.sqrt(var_sr) * ((1 - EULER_GAMMA) * norm.ppf(1 - 1 / N) +
                             EULER_GAMMA * norm.ppf(1 - 1 / (N * np.e))) if N > 1 else 0.0
    sk, ku = skew(r), kurtosis(r, fisher=False)
    # вариант с теоретической дисперсией Sharpe при H0 (≈1/T) — менее консервативный, для прозрачности
    sr0_null = np.sqrt(1.0 / T) * ((1 - EULER_GAMMA) * norm.ppf(1 - 1 / N) +
                                   EULER_GAMMA * norm.ppf(1 - 1 / (N * np.e))) if N > 1 else 0.0
    return {"SR_daily": float(sr), "SR_ann": float(sr * np.sqrt(365)), "N_trials": N,
            "DSR_nullvar": probabilistic_sharpe(sr, sr0_null, T, sk, ku),
            "SR0_daily": float(sr0), "SR0_ann": float(sr0 * np.sqrt(365)),
            "PSR(>0)": probabilistic_sharpe(sr, 0.0, T, sk, ku),
            "DSR": probabilistic_sharpe(sr, sr0, T, sk, ku), "T_days": T}


def whites_reality_check(R: pd.DataFrame, bench: pd.Series | None = None, n_boot: int = 2000,
                         block: float = 10, seed: int = SEED) -> dict:
    """H0: ни одна из K стратегий не лучше бенчмарка (по средней дневной доходности).
    R — T×K дневные доходности всех испытанных конфигураций (OOS)."""
    R = R.dropna(how="all").fillna(0.0)
    if bench is not None:
        R = R.sub(bench.reindex(R.index).fillna(0.0), axis=0)
    X = R.to_numpy()
    T, K = X.shape
    if T < 30:
        return {"p_value": np.nan, "note": "мало наблюдений"}
    mu = X.mean(axis=0)
    V = np.sqrt(T) * mu.max()
    rng = np.random.default_rng(seed)
    cnt = 0
    for _ in range(n_boot):
        Xb = X[stationary_bootstrap_idx(T, block, rng)]
        Vb = np.sqrt(T) * (Xb.mean(axis=0) - mu).max()
        cnt += Vb >= V
    best = int(np.argmax(mu))
    return {"K": K, "T_days": T, "best": R.columns[best], "best_mean_daily": float(mu[best]),
            "p_value": cnt / n_boot}


# --------------------------------------------------------- случайные входы
def random_entries(df: pd.DataFrame, f: pd.DataFrame, funding, cfg: ResearchConfig, ref: BTResult,
                   n_sims: int = 300, seed: int = SEED) -> dict:
    """Та же частота сделок, то же распределение длительностей и доля лонгов, те же издержки."""
    t = ref.trades
    if len(t) < 5:
        return {"note": "мало сделок"}
    rng = np.random.default_rng(seed)
    n = len(df)
    holds = t.bars.to_numpy()
    p_long = float((t.side > 0).mean())
    ref_ret = ref.equity.iloc[-1] / ref.equity.iloc[0] - 1
    ref_sh = _sharpe(daily_returns(ref.equity))
    rets, shs = [], []
    for _ in range(n_sims):
        le = np.zeros(n, bool); se = np.zeros(n, bool); lx = np.zeros(n, bool); sx = np.zeros(n, bool)
        starts = np.sort(rng.choice(np.arange(200, n - 2), size=min(len(t), n // 3), replace=False))
        busy_until = -1
        for s0 in starts:
            if s0 <= busy_until:
                continue
            h = int(rng.choice(holds))
            (le if rng.random() < p_long else se)[s0] = True
            busy_until = s0 + h
        sig = pd.DataFrame({"long_entry": le, "short_entry": se, "long_exit": lx, "short_exit": sx,
                            "stop_atr": 2.5, "tp_atr": np.nan, "max_hold": float(np.median(holds)),
                            "size_mult": 1.0}, index=df.index)
        r = run_backtest(df, sig, f["atr"], cfg.base_tf, funding, cfg.costs, cfg.risk)
        rets.append(r.equity.iloc[-1] / r.equity.iloc[0] - 1)
        shs.append(_sharpe(daily_returns(r.equity)))
    shs = np.array(shs, float)
    return {"random_ret_p50": float(np.median(rets)), "random_sharpe_p50": float(np.nanmedian(shs)),
            "random_sharpe_p95": float(np.nanquantile(shs, .95)),
            "strategy_sharpe": ref_sh, "strategy_ret": float(ref_ret),
            "p_value(random>=strategy)": float(np.nanmean(shs >= ref_sh))}
