"""
research.py — нестандартные гипотезы (этап 4). Каждая проверяется ОДИНАКОВО:
  1) гипотеза формулируется до теста;
  2) параметры/пороги (если есть) — только на IS (первые 1-oos_fraction истории);
  3) вердикт — по OOS: информационный коэффициент (IC) с HAC/Newey-West t-stat
     и, где применимо, торговый результат ПОСЛЕ издержек.
Гипотеза «работает», только если знак эффекта совпадает в IS и OOS и |t_OOS| > 2.
"""
from __future__ import annotations

import numpy as np
import pandas as pd
import statsmodels.api as sm

from config import SEED, TF_PANDAS, ResearchConfig


# ------------------------------------------------------------------- утилиты
def fwd_return(close: pd.Series, h: int) -> pd.Series:
    """Доходность от close[t] до close[t+h] (метка — будущее, используется только как y)."""
    return np.log(close.shift(-h) / close)


def hac_ic(x: pd.Series, y: pd.Series, lags: int) -> dict:
    """Регрессия y ~ x со стандартизацией, HAC t-stat (перекрывающиеся окна -> автокорреляция)."""
    d = pd.concat([x, y], axis=1).dropna()
    if len(d) < 50 or d.iloc[:, 0].std() == 0:
        return {"n": len(d), "ic": np.nan, "t": np.nan}
    xs = (d.iloc[:, 0] - d.iloc[:, 0].mean()) / d.iloc[:, 0].std()
    ys = (d.iloc[:, 1] - d.iloc[:, 1].mean()) / d.iloc[:, 1].std()
    m = sm.OLS(ys.values, sm.add_constant(xs.values)).fit(cov_type="HAC", cov_kwds={"maxlags": lags})
    return {"n": len(d), "ic": float(m.params[1]), "t": float(m.tvalues[1])}


def split_is_oos(idx: pd.Index, oos_fraction: float) -> tuple[pd.Index, pd.Index]:
    cut = int(len(idx) * (1 - oos_fraction))
    return idx[:cut], idx[cut:]


def verdict(is_: dict, oos: dict) -> str:
    if not np.isfinite(oos.get("t", np.nan)):
        return "недостаточно данных"
    same = np.sign(is_.get("ic", 0)) == np.sign(oos.get("ic", 0))
    if same and abs(oos["t"]) > 2 and abs(is_.get("t", 0)) > 2:
        return "ПОДТВЕРЖДЕНО OOS"
    if same and abs(oos["t"]) > 1.5:
        return "слабое, не значимо"
    return "НЕ подтверждено"


def ic_test(name: str, x: pd.Series, close: pd.Series, h: int, oos_fraction: float) -> dict:
    y = fwd_return(close, h)
    i_is, i_oos = split_is_oos(close.index, oos_fraction)
    a, b = hac_ic(x.loc[i_is], y.loc[i_is], h), hac_ic(x.loc[i_oos], y.loc[i_oos], h)
    return {"hypothesis": name, "horizon": h, "IC_IS": a["ic"], "t_IS": a["t"], "n_IS": a["n"],
            "IC_OOS": b["ic"], "t_OOS": b["t"], "n_OOS": b["n"], "verdict": verdict(a, b)}


# ---------------------------------------- 1. funding как контрарный индикатор
def test_funding_contrarian(f, close, cfg: ResearchConfig) -> list[dict]:
    if "funding_z" not in f or f["funding_z"].isna().all():
        return [{"hypothesis": "funding contrarian", "verdict": "нет данных funding"}]
    # H: высокий funding z -> отрицательная будущая доходность (IC < 0)
    return [ic_test("funding_z -> fwd ret (ожидаем IC<0)", f["funding_z"], close, h, cfg.oos_fraction)
            for h in (8, 24, 72)]


# ------------------------------------------------ 2. OI + цена + объём
def test_oi_price(f, close, cfg: ResearchConfig) -> pd.DataFrame:
    """Средняя будущая доходность (24 бара) по квадрантам OI/цена, IS vs OOS."""
    if "oi_px_quadrant" not in f or f["oi_px_quadrant"].isna().all():
        return pd.DataFrame({"note": ["нет данных OI"]})
    y = fwd_return(close, 24)
    names = {3: "цена↑ OI↑ (новые лонги)", 1: "цена↑ OI↓ (закрытие шортов)",
             -1: "цена↓ OI↑ (новые шорты)", -3: "цена↓ OI↓ (закрытие лонгов)"}
    i_is, i_oos = split_is_oos(close.index, cfg.oos_fraction)
    rows = []
    for q, nm in names.items():
        for lab, ix in (("IS", i_is), ("OOS", i_oos)):
            m = f.loc[ix, "oi_px_quadrant"] == q
            yy = y.loc[ix][m].dropna()
            # только сильные движения объёма (vol_z>1) — «кто двигает рынок»
            mv = m & (f.loc[ix, "vol_z"] > 1)
            yv = y.loc[ix][mv].dropna()
            rows.append({"quadrant": nm, "sample": lab, "n": len(yy), "mean_fwd24_bps": yy.mean() * 1e4,
                         "t_naive": yy.mean() / (yy.std() / np.sqrt(max(len(yy) / 24, 1))) if len(yy) > 30 else np.nan,
                         "n_highvol": len(yv), "mean_fwd24_highvol_bps": yv.mean() * 1e4})
    return pd.DataFrame(rows)


# ------------------------------------ 3. «охота за ликвидностью» (прокси)
def liquidation_heat(df: pd.DataFrame, oi: pd.Series, leverages=(10, 25, 50), mmr: float = 0.01,
                     band: float = 0.05, decay: float = 0.995, nbins: int = 400) -> pd.Series:
    """ПРИБЛИЖЕНИЕ (исторической карты ликвидаций в открытом доступе нет):
    прирост OI в баре считаем новыми позициями 50/50 long/short по типичной цене бара
    с плечами `leverages`; их цены ликвидации копим в гистограмме, масса «сгорает»,
    когда цена её пересекает, и экспоненциально затухает. Признак = (масса выше - масса ниже)
    в полосе ±band / общая масса в полосе. >0 — «магнит» сверху."""
    tp = ((df["high"] + df["low"] + df["close"]) / 3).to_numpy()
    d_oi = oi.reindex(df.index).diff().clip(lower=0).fillna(0).to_numpy()
    lo_p, hi_p = np.nanmin(df["low"]) * 0.5, np.nanmax(df["high"]) * 1.5
    edges = np.geomspace(lo_p, hi_p, nbins + 1)
    mass = np.zeros(nbins)
    out = np.full(len(df), np.nan)
    hi, lo, c = df["high"].to_numpy(), df["low"].to_numpy(), df["close"].to_numpy()
    for i in range(len(df)):
        mass *= decay
        # ликвидированы уровни, через которые прошла цена бара
        a, b = np.searchsorted(edges, lo[i]) - 1, np.searchsorted(edges, hi[i])
        mass[max(a, 0):min(b, nbins)] = 0.0
        if d_oi[i] > 0 and np.isfinite(tp[i]):
            for L in leverages:
                for px in (tp[i] * (1 - 1 / L + mmr), tp[i] * (1 + 1 / L - mmr)):
                    k = np.searchsorted(edges, px) - 1
                    if 0 <= k < nbins:
                        mass[k] += d_oi[i] / (2 * len(leverages))
        j0, j1 = np.searchsorted(edges, c[i] * (1 - band)) - 1, np.searchsorted(edges, c[i] * (1 + band))
        jc = np.searchsorted(edges, c[i]) - 1
        above, below = mass[jc + 1:j1].sum(), mass[max(j0, 0):jc].sum()
        tot = above + below
        out[i] = (above - below) / tot if tot > 0 else 0.0
    return pd.Series(out, df.index, name="liq_magnet")


def test_liquidity_magnet(df, f, cfg: ResearchConfig) -> list[dict]:
    if "oi" not in f or f["oi"].isna().all():
        return [{"hypothesis": "liquidation magnet", "verdict": "нет данных OI"}]
    x = liquidation_heat(df, f["oi"], mmr=cfg.risk.maint_margin_rate)
    return [ic_test("liq_magnet -> fwd ret (ожидаем IC>0)", x, df["close"], h, cfg.oos_fraction)
            for h in (8, 24)]


# ------------------------------------------------------- 4. lead-lag с BTC
def test_lead_lag(ton15: pd.DataFrame, btc15: pd.DataFrame, cfg: ResearchConfig, max_lag: int = 4) -> dict:
    """На 15m: (a) кросс-корреляции r_TON(t) с r_BTC(t-k); (b) тест Грейнджера на IS;
    (c) торговое правило OOS: сильный бар BTC (>2σ), TON отстал -> вход TON на 1 бар
    в сторону BTC; издержки: 2×taker + 2×slippage."""
    from statsmodels.tsa.stattools import grangercausalitytests
    r = np.log(ton15["close"]).diff()
    rb = np.log(btc15["close"].reindex(ton15.index)).diff()
    d = pd.concat([r.rename("ton"), rb.rename("btc")], axis=1).dropna()
    i_is, i_oos = split_is_oos(d.index, cfg.oos_fraction)
    xc = {k: {"IS": d.loc[i_is, "ton"].corr(d.loc[i_is, "btc"].shift(k)),
              "OOS": d.loc[i_oos, "ton"].corr(d.loc[i_oos, "btc"].shift(k))} for k in range(0, max_lag + 1)}
    try:
        g = grangercausalitytests(d.loc[i_is, ["ton", "btc"]].values, maxlag=max_lag)
        gp = {int(k): float(v[0]["ssr_ftest"][1]) for k, v in g.items()}
    except Exception as e:  # noqa: BLE001
        gp = {"error": str(e)}
    sd = d["btc"].rolling(96 * 7).std()
    beta = d["ton"].rolling(96 * 7).cov(d["btc"]) / d["btc"].rolling(96 * 7).var()
    strong = d["btc"].abs() > 2 * sd.shift(1)
    lag = (d["ton"] * np.sign(d["btc"])) < 0.5 * beta.shift(1) * d["btc"].abs()
    sig = np.sign(d["btc"]).where(strong & lag, 0.0)
    nxt = np.log(ton15["open"].shift(-2) / ton15["open"].shift(-1)).reindex(d.index)  # вход на open t+1, выход open t+2
    cost = 2 * (cfg.costs.taker_fee + cfg.costs.slippage_bps * 1e-4)
    pnl = (sig * nxt - cost * (sig != 0)).dropna()
    res = {}
    for lab, ix in (("IS", i_is), ("OOS", i_oos)):
        p = pnl.loc[pnl.index.intersection(ix)]
        p = p[sig.loc[p.index] != 0]
        res[lab] = {"n": len(p), "mean_bps_net": p.mean() * 1e4 if len(p) else np.nan,
                    "mean_bps_gross": (p.mean() + cost) * 1e4 if len(p) else np.nan,
                    "t": p.mean() / (p.std() / np.sqrt(len(p))) if len(p) > 10 else np.nan}
    return {"xcorr": pd.DataFrame(xc).T, "granger_p_IS": gp, "rule": pd.DataFrame(res).T}


# ------------------------------------------- 5. Калман / вейвлеты (каузально)
def kalman_local_trend(y: pd.Series, q_level: float, q_slope: float, r: float) -> pd.DataFrame:
    """Фильтр Калмана local-linear-trend (только ФИЛЬТРАЦИЯ, без сглаживания = без look-ahead)."""
    F = np.array([[1, 1], [0, 1]]); H = np.array([[1, 0]])
    Q = np.diag([q_level, q_slope]); R = np.array([[r]])
    x = np.array([y.iloc[0], 0.0]); P = np.eye(2)
    lv, sl = np.empty(len(y)), np.empty(len(y))
    for i, v in enumerate(y.to_numpy()):
        x = F @ x; P = F @ P @ F.T + Q
        if np.isfinite(v):
            S = H @ P @ H.T + R
            K = P @ H.T / S
            x = x + (K * (v - H @ x)).ravel()
            P = (np.eye(2) - K @ H) @ P
        lv[i], sl[i] = x
    return pd.DataFrame({"kf_level": lv, "kf_slope": sl}, index=y.index)


def causal_wavelet(y: pd.Series, window: int = 128, wavelet: str = "db4", level: int = 3) -> pd.Series:
    """Вейвлет-денойзинг на скользящем окне [t-window+1, t]; берём ПОСЛЕДНЕЕ значение
    реконструкции (краевой эффект есть, но будущего нет). Полноисторический денойзинг
    — классический источник look-ahead, здесь он запрещён."""
    import pywt
    v = np.array(y.to_numpy(), dtype=float, copy=True)
    out = np.full(len(v), np.nan)
    for i in range(window - 1, len(v)):
        seg = v[i - window + 1:i + 1].copy()
        coeffs = pywt.wavedec(seg, wavelet, level=level, mode="symmetric")
        sigma = np.median(np.abs(coeffs[-1])) / 0.6745
        thr = sigma * np.sqrt(2 * np.log(len(seg)))
        coeffs[1:] = [pywt.threshold(c, thr, mode="soft") for c in coeffs[1:]]
        out[i] = pywt.waverec(coeffs, wavelet, mode="symmetric")[len(seg) - 1]
    return pd.Series(out, y.index)


def test_denoising(close: pd.Series, cfg: ResearchConfig, h: int = 24) -> pd.DataFrame:
    """Сравнение предсказательной силы наклона: EMA vs Калман vs вейвлет. Параметр Калмана
    (отношение q/r) выбирается на IS по IC, затем фиксируется."""
    y = np.log(close)
    i_is, _ = split_is_oos(close.index, cfg.oos_fraction)
    fr = fwd_return(close, h)
    var = y.diff().loc[i_is].var()
    best, best_ic = None, -np.inf
    for ratio in (1e-4, 1e-3, 1e-2):
        k = kalman_local_trend(y.loc[i_is], var * ratio, var * ratio * 1e-2, var)
        ic = abs(hac_ic(k["kf_slope"], fr.loc[i_is], h)["ic"])
        if np.isfinite(ic) and ic > best_ic:
            best, best_ic = ratio, ic
    kf = kalman_local_trend(y, var * best, var * best * 1e-2, var)
    ema_slope = y.ewm(span=48, adjust=False).mean().diff()
    wav = causal_wavelet(y).diff()
    rows = [ic_test("EMA48 slope", ema_slope, close, h, cfg.oos_fraction),
            ic_test(f"Kalman slope (q/r={best})", kf["kf_slope"], close, h, cfg.oos_fraction),
            ic_test("Wavelet(db4,L3,128) slope", wav, close, h, cfg.oos_fraction)]
    return pd.DataFrame(rows)


# ---------------------------------------------- 6. ML: gradient boosting vs простая модель
ML_FEATURES = ["rsi", "macd_hist_n", "stochrsi_k", "roc_24", "adx", "slope_t", "supertrend", "ema_20_50",
               "ema_50_200", "px_vs_sma200", "atr_pct", "bb_pctb", "bb_width", "rv_24", "rv_pctile", "squeeze",
               "obv_slope", "vwap_dev", "cvd_slope", "poc_dev", "vp_vol_above", "vol_z", "h4_trend",
               "funding_z", "oi_chg", "oi_z", "ls_accounts_z", "ls_top_positions_z", "taker_ls_vol_z",
               "basis_z", "beta_btc", "corr_btc", "btc_ret1", "rs_btc", "resid_ret_btc", "leadlag_btc_1",
               "hour", "dow"]


def ml_direction(f: pd.DataFrame, df: pd.DataFrame, cfg: ResearchConfig, h: int = 24,
                 thr: float = 0.55) -> dict:
    """Классификатор знака доходности open[t+1] -> open[t+1+h].
    Walk-forward (expanding train с purge+embargo) — модель никогда не видит будущее.
    Сравнение: HistGradientBoosting vs логистическая регрессия vs «всегда большинство»."""
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.inspection import permutation_importance
    from sklearn.linear_model import LogisticRegression
    from sklearn.metrics import accuracy_score, log_loss, roc_auc_score
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler

    cols = [c for c in ML_FEATURES if c in f.columns and f[c].notna().mean() > 0.5]
    X = f[cols].replace([np.inf, -np.inf], np.nan)
    ret = np.log(df["open"].shift(-(1 + h)) / df["open"].shift(-1))
    y = (ret > 0).astype(float).where(ret.notna())
    ok = y.notna() & X.notna().mean(axis=1).gt(0.8)
    X, y, ret = X[ok], y[ok].astype(int), ret[ok]
    n = len(X)
    wf = cfg.wf
    emb = h + int(0.01 * n)
    preds = {"gb": pd.Series(np.nan, X.index), "logit": pd.Series(np.nan, X.index)}
    base_p = pd.Series(np.nan, X.index)
    imps = []
    start = wf.train_bars
    while start < n:
        end = min(start + wf.test_bars, n)
        tr = np.arange(0, max(start - emb, 0))            # purge: метки train не заходят в test
        te = np.arange(start, end)
        if len(tr) < 1000:
            start = end
            continue
        Xtr, ytr = X.iloc[tr], y.iloc[tr]
        gb = HistGradientBoostingClassifier(max_depth=3, learning_rate=0.05, max_iter=200,
                                            l2_regularization=1.0, min_samples_leaf=200, random_state=SEED)
        gb.fit(Xtr, ytr)
        lr = make_pipeline(StandardScaler(), LogisticRegression(C=0.1, max_iter=500))
        lr.fit(Xtr.fillna(Xtr.median()), ytr)
        preds["gb"].iloc[te] = gb.predict_proba(X.iloc[te])[:, 1]
        preds["logit"].iloc[te] = lr.predict_proba(X.iloc[te].fillna(Xtr.median()))[:, 1]
        base_p.iloc[te] = ytr.mean()
        if len(te) > 200:
            pi = permutation_importance(gb, X.iloc[te], y.iloc[te], n_repeats=3, random_state=SEED,
                                        scoring="roc_auc")
            imps.append(pd.Series(pi.importances_mean, index=cols))
        start = end
    m = preds["gb"].notna()
    yt, rt = y[m], ret[m]
    out = {"n_oos": int(m.sum()), "features_used": cols}
    cost = 2 * (cfg.costs.taker_fee + cfg.costs.slippage_bps * 1e-4)
    rows = {}
    for name, p in (("gb", preds["gb"][m]), ("logit", preds["logit"][m]), ("base_rate", base_p[m])):
        pos = np.where(p > thr, 1, np.where(p < 1 - thr, -1, 0))
        # непересекающиеся сделки: берём каждый h-й бар, чтобы не завышать число наблюдений
        sel = np.arange(0, len(pos), h)
        tr_ret = pos[sel] * rt.to_numpy()[sel] - cost * (pos[sel] != 0)
        traded = tr_ret[pos[sel] != 0]
        rows[name] = {
            "AUC": roc_auc_score(yt, p) if p.nunique() > 1 else 0.5,
            "LogLoss": log_loss(yt, np.clip(p, 1e-6, 1 - 1e-6)),
            "Accuracy": accuracy_score(yt, (p > 0.5).astype(int)),
            "trades(non-overlap)": int((pos[sel] != 0).sum()),
            "mean_trade_bps_net": float(traded.mean() * 1e4) if len(traded) else np.nan,
            "t_net": float(traded.mean() / (traded.std() / np.sqrt(len(traded)))) if len(traded) > 10 else np.nan}
    out["table"] = pd.DataFrame(rows).T
    out["importance"] = pd.concat(imps, axis=1).mean(axis=1).sort_values(ascending=False) if imps else pd.Series(dtype=float)
    out["majority_rate"] = float(yt.mean())
    return out


# ---------------------------------------------------------- 7. event study
def event_study(daily: pd.DataFrame, btc_daily: pd.DataFrame, events: pd.DataFrame,
                pre: int = 3, post: int = 10, est: int = 60) -> dict:
    """Рыночная модель r_TON = a + b r_BTC на окне [-est-pre, -pre), аномальная доходность AR
    и кумулятивная CAR на [-pre, +post]. При малом числе событий мощность теста низкая."""
    r = np.log(daily["close"]).diff()
    rb = np.log(btc_daily["close"].reindex(daily.index)).diff()
    rows, paths = [], {}
    for _, e in events.iterrows():
        t0 = pd.Timestamp(e["date"], tz="UTC").normalize()
        if t0 not in r.index:
            continue
        k = r.index.get_loc(t0)
        if k - pre - est < 1 or k + post >= len(r):
            continue
        ei = r.index[k - pre - est:k - pre]
        X = sm.add_constant(rb.loc[ei].values)
        mdl = sm.OLS(r.loc[ei].values, X, missing="drop").fit()
        win = r.index[k - pre:k + post + 1]
        ar = r.loc[win] - (mdl.params[0] + mdl.params[1] * rb.loc[win])
        sd = np.std(mdl.resid, ddof=2)
        car = ar.cumsum()
        paths[e["event"]] = pd.Series(car.values, index=range(-pre, post + 1))
        rows.append({"date": t0.date(), "event": e["event"], "type": e.get("type", ""),
                     "CAR[-3,0]": float(ar.iloc[:pre + 1].sum()), "CAR[0,+3]": float(ar.iloc[pre:pre + 4].sum()),
                     f"CAR[-{pre},+{post}]": float(car.iloc[-1]),
                     "t_CAR": float(car.iloc[-1] / (sd * np.sqrt(len(win))))})
    tab = pd.DataFrame(rows)
    summary = {}
    if len(tab):
        for typ, g in tab.groupby("type"):
            v = g[f"CAR[-{pre},+{post}]"]
            summary[typ] = {"n": len(g), "mean_CAR": v.mean(),
                            "t": v.mean() / (v.std() / np.sqrt(len(v))) if len(v) > 2 and v.std() > 0 else np.nan}
    return {"events": tab, "by_type": pd.DataFrame(summary).T, "paths": pd.DataFrame(paths)}


# ------------------------------------------------ 8. специфика TON: часы, скачки
def hour_of_day_profile(df: pd.DataFrame) -> pd.DataFrame:
    """Ликвидность по часам UTC: доля объёма, |доходность|, Amihud-неликвидность (|r|/$vol)."""
    r = np.log(df["close"]).diff().abs()
    qv = df["quote_volume"] if "quote_volume" in df else df["volume"] * df["close"]
    g = pd.DataFrame({"abs_ret_bps": r * 1e4, "qvol": qv, "amihud": r / (qv + 1e-9) * 1e6,
                      "range_bps": (df["high"] / df["low"] - 1) * 1e4}).groupby(df.index.hour)
    out = g.mean()
    out["vol_share"] = g["qvol"].sum() / g["qvol"].sum().sum()
    out["amihud_rel"] = out["amihud"] / out["amihud"].median()
    return out


def jump_analysis(df: pd.DataFrame, n: int = 24 * 30, k: float = 5.0) -> dict:
    """Скачки: |r| > k·σ (σ — скользящая, по прошлому). Частота, по часам, отдача после скачка."""
    r = np.log(df["close"]).diff()
    sd = r.rolling(n, min_periods=n // 2).std().shift(1)
    j = r.abs() > k * sd
    after = np.log(df["close"].shift(-24) / df["close"])
    jt = pd.DataFrame({"ret": r[j], "sigma": (r / sd)[j], "fwd24": after[j]})
    return {"n_jumps": int(j.sum()), "per_1000_bars": float(j.mean() * 1000),
            "normal_expected_per_1000": float(2 * (1 - __import__("scipy").stats.norm.cdf(k)) * 1000),
            "by_hour": j.groupby(df.index.hour).sum(),
            "continuation_share": float((np.sign(jt.ret) == np.sign(jt.fwd24)).mean()) if len(jt) else np.nan,
            "jumps": jt}


# --------------------------------------------------- гипотеза H1 (тренд)
def trend_hypothesis(daily: pd.DataFrame, btc_daily: pd.DataFrame | None = None) -> dict:
    """H1: «нейтрально-положительный тренд». Операционализация (заявлена ДО теста):
      * наклон лог-цены за 90 дн. > 0 и его t-stat (HAC) не ниже -1, но и не обязателен > 2;
      * цена выше SMA200 или SMA50 > SMA200 хотя бы по одному критерию;
      * доходность 30/90/365 дн., положение в диапазоне.
    Вердикт: подтверждена — наклон90 > 0 c t>1 И цена > SMA200;
             опровергнута — наклон90 < 0 c t<-1 И цена < SMA200;
             иначе — недостаточно данных/смешанная картина."""
    c = daily["close"].dropna()
    out = {"last_date": str(c.index[-1].date()), "last_close": float(c.iloc[-1])}
    for d in (30, 90, 365):
        if len(c) > d:
            w = c.iloc[-d:]
            out[f"ret_{d}d"] = float(c.iloc[-1] / c.iloc[-d - 1] - 1)
            out[f"range_{d}d"] = (float(w.min()), float(w.max()))
            out[f"pos_in_range_{d}d"] = float((c.iloc[-1] - w.min()) / (w.max() - w.min() + 1e-12))
            y = np.log(w.values)
            X = sm.add_constant(np.arange(len(y)))
            m = sm.OLS(y, X).fit(cov_type="HAC", cov_kwds={"maxlags": 5})
            out[f"slope_{d}d_pct_per_day"] = float(m.params[1] * 100)
            out[f"slope_{d}d_t"] = float(m.tvalues[1])
    s50, s200 = c.rolling(50).mean().iloc[-1], c.rolling(200).mean().iloc[-1]
    out.update({"sma50": float(s50), "sma200": float(s200), "above_sma200": bool(c.iloc[-1] > s200),
                "sma50_gt_sma200": bool(s50 > s200)})
    t90, sl90 = out.get("slope_90d_t", np.nan), out.get("slope_90d_pct_per_day", np.nan)
    if np.isfinite(t90) and sl90 > 0 and t90 > 1 and out["above_sma200"]:
        v = "ПОДТВЕРЖДЕНА"
    elif np.isfinite(t90) and sl90 < 0 and t90 < -1 and not out["above_sma200"]:
        v = "ОПРОВЕРГНУТА"
    else:
        v = "НЕДОСТАТОЧНО ДАННЫХ / смешанная картина"
    out["verdict_H1"] = v
    if btc_daily is not None and len(btc_daily):
        rb = np.log(btc_daily["close"].reindex(c.index)).diff()
        r = np.log(c).diff()
        out["corr_btc_90d"] = float(r.iloc[-90:].corr(rb.iloc[-90:]))
        out["beta_btc_90d"] = float(r.iloc[-90:].cov(rb.iloc[-90:]) / rb.iloc[-90:].var())
        out["ton_vs_btc_90d"] = float(out.get("ret_90d", np.nan) -
                                      (btc_daily["close"].iloc[-1] / btc_daily["close"].iloc[-91] - 1))
    return out


def support_resistance(daily: pd.DataFrame, lookback: int = 180, bins: int = 60, top: int = 6) -> pd.DataFrame:
    """Уровни = пики объёмного профиля (дневные бары, окно lookback) + свинговые экстремумы."""
    d = daily.iloc[-lookback:]
    tp = (d["high"] + d["low"] + d["close"]) / 3
    h, e = np.histogram(tp, bins=bins, weights=d["volume"] * tp)
    centers = 0.5 * (e[1:] + e[:-1])
    peaks = [i for i in range(1, bins - 1) if h[i] >= h[i - 1] and h[i] >= h[i + 1]]
    peaks = sorted(peaks, key=lambda i: -h[i])[:top]
    last = d["close"].iloc[-1]
    rows = [{"level": centers[i], "kind": "volume node", "weight": h[i] / h.max(),
             "side": "resistance" if centers[i] > last else "support"} for i in peaks]
    for w in (20, 60):
        rows.append({"level": d["high"].iloc[-w:].max(), "kind": f"{w}d high", "weight": np.nan, "side": "resistance"})
        rows.append({"level": d["low"].iloc[-w:].min(), "kind": f"{w}d low", "weight": np.nan, "side": "support"})
    return pd.DataFrame(rows).sort_values("level", ascending=False).reset_index(drop=True)


def bars_per_day(tf: str) -> int:
    return int(pd.Timedelta("1D") / pd.Timedelta(TF_PANDAS[tf]))
