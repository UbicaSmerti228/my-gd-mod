"""
features.py — признаки. ВСЕ признаки каузальны: значение в баре t использует
только данные, известные на момент ЗАКРЫТИЯ бара t (проверяется тестом
tests/test_no_lookahead.py). Внешние ряды (funding, OI, старшие ТФ) выравниваются
через merge_asof по времени закрытия бара.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from config import TF_PANDAS

EPS = 1e-12


# ----------------------------------------------------------------- базовые блоки
def ema(s: pd.Series, n: int) -> pd.Series:
    return s.ewm(span=n, adjust=False, min_periods=n).mean()


def sma(s: pd.Series, n: int) -> pd.Series:
    return s.rolling(n, min_periods=n).mean()


def wilder(s: pd.Series, n: int) -> pd.Series:
    return s.ewm(alpha=1.0 / n, adjust=False, min_periods=n).mean()


def true_range(df: pd.DataFrame) -> pd.Series:
    pc = df["close"].shift(1)
    return pd.concat([df["high"] - df["low"], (df["high"] - pc).abs(), (df["low"] - pc).abs()], axis=1).max(axis=1)


def atr(df: pd.DataFrame, n: int = 14) -> pd.Series:
    return wilder(true_range(df), n)


def zscore(s: pd.Series, n: int) -> pd.Series:
    m, sd = s.rolling(n, min_periods=n // 2).mean(), s.rolling(n, min_periods=n // 2).std()
    return (s - m) / (sd + EPS)


# ----------------------------------------------------------------------- тренд
def adx(df: pd.DataFrame, n: int = 14) -> pd.DataFrame:
    up, dn = df["high"].diff(), -df["low"].diff()
    plus_dm = np.where((up > dn) & (up > 0), up, 0.0)
    minus_dm = np.where((dn > up) & (dn > 0), dn, 0.0)
    tr = wilder(true_range(df), n)
    pdi = 100 * wilder(pd.Series(plus_dm, df.index), n) / (tr + EPS)
    mdi = 100 * wilder(pd.Series(minus_dm, df.index), n) / (tr + EPS)
    dx = 100 * (pdi - mdi).abs() / (pdi + mdi + EPS)
    return pd.DataFrame({"adx": wilder(dx, n), "pdi": pdi, "mdi": mdi})


def regression_slope(s: pd.Series, n: int) -> pd.DataFrame:
    """Наклон OLS log-цены на окне n и его t-статистика (вектор. через rolling cov)."""
    y = np.log(s)
    x = pd.Series(np.arange(len(s), dtype=float), s.index)
    cov = y.rolling(n).cov(x)
    varx = x.rolling(n).var()
    slope = cov / varx
    resid_var = (y.rolling(n).var() - slope ** 2 * varx) * (n - 1) / max(n - 2, 1)
    se = np.sqrt(resid_var.clip(lower=0) / (varx * (n - 1)))
    return pd.DataFrame({"slope": slope, "slope_t": slope / (se + EPS)})


def supertrend(df: pd.DataFrame, n: int = 10, mult: float = 3.0) -> pd.Series:
    """Классический Supertrend; возвращает направление +1/-1."""
    a = atr(df, n).values
    hl2 = ((df["high"] + df["low"]) / 2).values
    close = df["close"].values
    ub, lb = hl2 + mult * a, hl2 - mult * a
    fub, flb = ub.copy(), lb.copy()
    direction = np.zeros(len(df))
    for i in range(1, len(df)):
        if np.isnan(a[i]):
            continue
        fub[i] = ub[i] if (ub[i] < fub[i - 1] or close[i - 1] > fub[i - 1]) else fub[i - 1]
        flb[i] = lb[i] if (lb[i] > flb[i - 1] or close[i - 1] < flb[i - 1]) else flb[i - 1]
        if direction[i - 1] <= 0:
            direction[i] = 1 if close[i] > fub[i - 1] else -1
        else:
            direction[i] = -1 if close[i] < flb[i - 1] else 1
    out = pd.Series(direction, df.index)
    return out.where(~np.isnan(a), np.nan)


# ---------------------------------------------------------------------- импульс
def rsi(s: pd.Series, n: int = 14) -> pd.Series:
    d = s.diff()
    g, l = wilder(d.clip(lower=0), n), wilder(-d.clip(upper=0), n)
    return 100 - 100 / (1 + g / (l + EPS))


def macd(s: pd.Series, f: int = 12, sl: int = 26, sig: int = 9) -> pd.DataFrame:
    m = ema(s, f) - ema(s, sl)
    sg = ema(m, sig)
    return pd.DataFrame({"macd": m, "macd_signal": sg, "macd_hist": m - sg})


def stoch_rsi(s: pd.Series, n: int = 14, k: int = 3, d: int = 3) -> pd.DataFrame:
    r = rsi(s, n)
    lo, hi = r.rolling(n).min(), r.rolling(n).max()
    st = (r - lo) / (hi - lo + EPS)
    kk = st.rolling(k).mean()
    return pd.DataFrame({"stochrsi_k": kk, "stochrsi_d": kk.rolling(d).mean()})


# ----------------------------------------------------------------- волатильность
def bollinger(s: pd.Series, n: int = 20, k: float = 2.0) -> pd.DataFrame:
    m, sd = sma(s, n), s.rolling(n).std()
    up, lo = m + k * sd, m - k * sd
    return pd.DataFrame({"bb_mid": m, "bb_up": up, "bb_lo": lo,
                         "bb_pctb": (s - lo) / (up - lo + EPS), "bb_width": (up - lo) / (m + EPS)})


def keltner(df: pd.DataFrame, n: int = 20, k: float = 1.5) -> pd.DataFrame:
    m, a = ema(df["close"], n), atr(df, n)
    return pd.DataFrame({"kc_mid": m, "kc_up": m + k * a, "kc_lo": m - k * a})


def realized_vol(s: pd.Series, n: int, bars_per_year: float) -> pd.Series:
    return np.log(s).diff().rolling(n).std() * np.sqrt(bars_per_year)


# ------------------------------------------------------------------------ объём
def obv(df: pd.DataFrame) -> pd.Series:
    return (np.sign(df["close"].diff()).fillna(0) * df["volume"]).cumsum()


def rolling_vwap(df: pd.DataFrame, n: int) -> pd.Series:
    tp = (df["high"] + df["low"] + df["close"]) / 3
    return (tp * df["volume"]).rolling(n).sum() / (df["volume"].rolling(n).sum() + EPS)


def session_vwap(df: pd.DataFrame) -> pd.Series:
    """VWAP, заякоренный на начало UTC-суток."""
    tp = (df["high"] + df["low"] + df["close"]) / 3
    day = df.index.floor("D")
    pv = (tp * df["volume"]).groupby(day).cumsum()
    v = df["volume"].groupby(day).cumsum()
    return pv / (v + EPS)


def cvd(df: pd.DataFrame) -> pd.Series:
    """Cumulative volume delta из taker-buy объёма (Binance klines). Без него — NaN
    (не подменяем эвристикой, чтобы не выдавать приближение за данные)."""
    if "taker_buy_volume" not in df.columns:
        return pd.Series(np.nan, df.index)
    delta = 2 * df["taker_buy_volume"] - df["volume"]
    return delta.cumsum()


def volume_profile_poc(df: pd.DataFrame, n: int = 168, bins: int = 30) -> pd.DataFrame:
    """Скользящий volume profile: POC (цена макс. объёма) и доля объёма выше/ниже цены.
    Каузально: окно [t-n+1, t]."""
    tp = ((df["high"] + df["low"] + df["close"]) / 3).values
    vol, close = df["volume"].values, df["close"].values
    poc = np.full(len(df), np.nan)
    above = np.full(len(df), np.nan)
    for i in range(n - 1, len(df)):
        p, v = tp[i - n + 1:i + 1], vol[i - n + 1:i + 1]
        lo, hi = p.min(), p.max()
        if hi <= lo:
            continue
        h, edges = np.histogram(p, bins=bins, range=(lo, hi), weights=v)
        j = int(np.argmax(h))
        poc[i] = 0.5 * (edges[j] + edges[j + 1])
        tot = v.sum()
        above[i] = v[p > close[i]].sum() / tot if tot > 0 else np.nan
    return pd.DataFrame({"vp_poc": poc, "vp_vol_above": above}, index=df.index)


# ------------------------------------------------------------ выравнивание рядов
def bar_close_times(idx: pd.DatetimeIndex, tf: str) -> pd.DatetimeIndex:
    return idx + pd.Timedelta(TF_PANDAS[tf])


def asof_align(series: pd.Series | pd.DataFrame, idx: pd.DatetimeIndex, tf: str,
               tolerance: pd.Timedelta | None = None) -> pd.DataFrame:
    """Для каждого бара берём последнее значение внешнего ряда с меткой <= закрытию бара."""
    if series is None or len(series) == 0:
        return pd.DataFrame(index=idx)
    ext = series.to_frame() if isinstance(series, pd.Series) else series
    ext = ext.sort_index()
    left = pd.DataFrame({"_t": bar_close_times(idx, tf)}, index=idx)
    right = ext.reset_index().rename(columns={ext.index.name or "index": "_t"})
    right["_t"] = pd.to_datetime(right["_t"], utc=True).astype(left["_t"].dtype)
    m = pd.merge_asof(left.reset_index(), right, on="_t", direction="backward", tolerance=tolerance)
    m = m.set_index(m.columns[0]).drop(columns="_t")
    m.index = idx
    return m


def align_htf(htf_df: pd.DataFrame, htf: str, idx: pd.DatetimeIndex, tf: str) -> pd.DataFrame:
    """Признаки старшего ТФ: значение становится доступно в момент ЗАКРЫТИЯ htf-бара."""
    shifted = htf_df.copy()
    shifted.index = shifted.index + pd.Timedelta(TF_PANDAS[htf])  # метка = время закрытия
    return asof_align(shifted, idx, tf)


# ----------------------------------------------------------------- деривативы
def derivatives_features(idx: pd.DatetimeIndex, tf: str, close: pd.Series,
                         funding: pd.Series | None, metrics: pd.DataFrame | None,
                         premium: pd.Series | None, z_n: int = 90) -> pd.DataFrame:
    out = pd.DataFrame(index=idx)
    if funding is not None and len(funding):
        f = funding.sort_index()
        fz = (f - f.rolling(z_n, min_periods=20).mean()) / (f.rolling(z_n, min_periods=20).std() + EPS)
        a = asof_align(pd.DataFrame({"funding": f, "funding_z": fz,
                                     "funding_ma3": f.rolling(3).mean()}), idx, tf,
                       tolerance=pd.Timedelta("1D"))
        out = out.join(a)
    if metrics is not None and len(metrics):
        m = asof_align(metrics, idx, tf, tolerance=pd.Timedelta("2h"))
        bars_day = int(pd.Timedelta("1D") / pd.Timedelta(TF_PANDAS[tf]))
        if "oi" in m:
            out["oi"] = m["oi"]
            k = max(1, bars_day // 6)            # ~4 часа
            out["oi_chg"] = np.log(m["oi"]).diff(k)
            out["px_chg"] = np.log(close).diff(k)
            # квадранты: +1/+1 новые лонги, -1/+1 новые шорты, +1/-1 закрытие шортов (short covering),
            # -1/-1 закрытие лонгов (long liquidation/капитуляция)
            out["oi_px_quadrant"] = (np.sign(out["px_chg"]) * 2 + np.sign(out["oi_chg"])).astype(float)
            out["oi_z"] = zscore(out["oi_chg"], bars_day * 30)
        for c in ("ls_accounts", "ls_top_positions", "taker_ls_vol"):
            if c in m:
                out[c] = m[c]
                out[c + "_z"] = zscore(np.log(m[c].clip(lower=EPS)), bars_day * 30)
    if premium is not None and len(premium):
        p = premium.reindex(idx)  # premium index kline с тем же индексом (время открытия)
        out["basis"] = p
        out["basis_z"] = zscore(p, int(pd.Timedelta("30D") / pd.Timedelta(TF_PANDAS[tf])))
    return out


# ----------------------------------------------------------------- кросс-актив
def cross_asset_features(df: pd.DataFrame, btc: pd.DataFrame | None, eth: pd.DataFrame | None,
                         n_beta: int = 24 * 30, max_lag: int = 3) -> pd.DataFrame:
    out = pd.DataFrame(index=df.index)
    r = np.log(df["close"]).diff()
    for name, b in (("btc", btc), ("eth", eth)):
        if b is None or not len(b):
            continue
        bc = b["close"].reindex(df.index)
        rb = np.log(bc).diff()
        cov = r.rolling(n_beta).cov(rb)
        out[f"beta_{name}"] = cov / (rb.rolling(n_beta).var() + EPS)
        out[f"corr_{name}"] = r.rolling(n_beta).corr(rb)
        out[f"{name}_ret1"] = rb
        out[f"{name}_trend"] = np.sign(ema(bc, 50) - ema(bc, 200))
        # lead-lag: корреляция TON_t с BTC_{t-k} (k>0 = BTC опережает)
        for k in range(1, max_lag + 1):
            out[f"leadlag_{name}_{k}"] = r.rolling(n_beta).corr(rb.shift(k))
        rs = np.log(df["close"] / bc)
        out[f"rs_{name}"] = rs - ema(rs, 50)                   # относительная сила TON/BTC
        # «остаточная» доходность: TON минус beta*BTC — идиосинкратическое движение
        out[f"resid_ret_{name}"] = r - out[f"beta_{name}"].shift(1) * rb
    return out


# ---------------------------------------------------------------- сборка всего
def build_features(panel: dict, tf: str | None = None) -> pd.DataFrame:
    tf = tf or panel["tf"]
    df = panel["perp"].copy()
    bars_year = pd.Timedelta("365D") / pd.Timedelta(TF_PANDAS[tf])
    bars_day = int(pd.Timedelta("1D") / pd.Timedelta(TF_PANDAS[tf]))
    c = df["close"]
    f = pd.DataFrame(index=df.index)
    f["ret1"] = np.log(c).diff()
    # тренд
    for a, b in ((20, 50), (50, 200)):
        f[f"ema_{a}_{b}"] = np.sign(ema(c, a) - ema(c, b))
    f["px_vs_sma200"] = c / sma(c, 200) - 1
    f = f.join(adx(df, 14))
    f = f.join(regression_slope(c, 48))
    f["supertrend"] = supertrend(df, 10, 3.0)
    # импульс
    f["rsi"] = rsi(c, 14)
    f = f.join(macd(c))
    f["macd_hist_n"] = f["macd_hist"] / c
    f = f.join(stoch_rsi(c))
    f["roc_24"] = c.pct_change(24)
    # волатильность
    f["atr"] = atr(df, 14)
    f["atr_pct"] = f["atr"] / c
    f = f.join(bollinger(c))
    f = f.join(keltner(df))
    f["rv_24"] = realized_vol(c, 24, bars_year)
    f["rv_168"] = realized_vol(c, 168, bars_year)
    f["rv_pctile"] = f["rv_168"].expanding(min_periods=24 * 30).rank(pct=True)  # каузальный перцентиль
    f["squeeze"] = ((f["bb_up"] < f["kc_up"]) & (f["bb_lo"] > f["kc_lo"])).astype(float)
    f["squeeze_release"] = ((f["squeeze"].shift(1) == 1) & (f["squeeze"] == 0)).astype(float)
    # объём
    f["obv_slope"] = zscore(obv(df).diff(24), bars_day * 30)
    f["vwap_dev"] = c / rolling_vwap(df, bars_day) - 1
    f["svwap_dev"] = c / session_vwap(df) - 1
    cv = cvd(df)
    f["cvd_slope"] = zscore(cv.diff(24), bars_day * 30) if cv.notna().any() else np.nan
    f = f.join(volume_profile_poc(df, n=bars_day * 7))
    f["poc_dev"] = c / f["vp_poc"] - 1
    f["vol_z"] = zscore(np.log(df["volume"] + 1), bars_day * 30)
    # старший ТФ: 4h тренд (доступен только после закрытия 4h-бара)
    if tf in ("15m", "1h"):
        from data import resample_ohlcv
        h4 = resample_ohlcv(df, "4h")
        h4f = pd.DataFrame({"h4_trend": np.sign(ema(h4["close"], 20) - ema(h4["close"], 50)),
                            "h4_adx": adx(h4, 14)["adx"]}, index=h4.index)
        f = f.join(align_htf(h4f, "4h", df.index, tf))
    # деривативы / кросс-актив
    f = f.join(derivatives_features(df.index, tf, c, panel.get("funding"), panel.get("metrics"),
                                    panel.get("premium")))
    f = f.join(cross_asset_features(df, panel.get("btc"), panel.get("eth"), n_beta=bars_day * 30))
    # календарь (для анализа ликвидности по часам)
    f["hour"] = df.index.hour
    f["dow"] = df.index.dayofweek
    f["gap_before"] = df["gap_before"].astype(bool)
    return f
