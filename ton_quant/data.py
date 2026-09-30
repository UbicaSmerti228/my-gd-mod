"""
data.py — загрузка и кэширование данных по бессрочным фьючерсам TON/GRAM.

Источники:
  1. Binance public data archive (data.binance.vision) — основной источник.
     Плюсы: содержит историю СНЯТЫХ контрактов (TONUSDT после ребрендинга),
     taker-buy объём (для CVD), funding, premium index (basis) и 5-минутные
     metrics (open interest, long/short ratio), которых нет в REST-истории.
  2. ccxt (Binance/Bybit/OKX REST) — альтернативный источник / свежие данные.

Всё кэшируется в parquet (CACHE_DIR). Данные НЕ синтезируются: если источник
недоступен, функция бросает DataUnavailable с описанием, чего не хватает.

Все временные метки — UTC, индекс = ВРЕМЯ ОТКРЫТИЯ бара.
"""
from __future__ import annotations

import io
import os
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

import numpy as np
import pandas as pd
import requests

from config import BENCH_SYMBOLS, CACHE_DIR, SYMBOL_HISTORY, TF_PANDAS

VISION = "https://data.binance.vision/data/futures/um"
KLINE_COLS = ["open_time", "open", "high", "low", "close", "volume", "close_time",
              "quote_volume", "count", "taker_buy_volume", "taker_buy_quote_volume", "ignore"]


class DataUnavailable(RuntimeError):
    pass


# --------------------------------------------------------------------------- utils
def _ts(x) -> pd.Timestamp | None:
    if x is None:
        return None
    t = pd.Timestamp(x)
    return t.tz_localize("UTC") if t.tzinfo is None else t.tz_convert("UTC")


def _to_utc_index(values) -> pd.DatetimeIndex:
    """ms/us epoch или строка -> DatetimeIndex UTC (Binance в 2025 перешёл на us в части архивов)."""
    s = pd.Series(values)
    if pd.api.types.is_numeric_dtype(s):
        s = s.astype("int64")
        unit = "us" if s.max() > 1e14 else "ms"
        return pd.DatetimeIndex(pd.to_datetime(s, unit=unit, utc=True))
    return pd.DatetimeIndex(pd.to_datetime(s, utc=True))


def _http_get(url: str, retries: int = 4) -> bytes | None:
    """GET с экспоненциальным backoff. 404 -> None (файла нет: символ ещё не торговался)."""
    delay = 2.0
    for i in range(retries):
        try:
            r = requests.get(url, timeout=30)
            if r.status_code == 404:
                return None
            r.raise_for_status()
            return r.content
        except requests.RequestException as e:
            if i == retries - 1:
                raise DataUnavailable(f"Не удалось скачать {url}: {e}") from e
            time.sleep(delay)
            delay *= 2
    return None


def _read_zip_csv(blob: bytes, columns: list[str] | None) -> pd.DataFrame:
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        raw = z.read(z.namelist()[0])
    first_tok = raw.split(b"\n", 1)[0].split(b",")[0].strip()
    has_header = not first_tok.replace(b".", b"").replace(b"-", b"").isdigit()
    df = pd.read_csv(io.BytesIO(raw), header=0 if has_header else None)
    if not has_header and columns is not None:
        df.columns = columns[: df.shape[1]]
    return df


def _months(start: pd.Timestamp, end: pd.Timestamp) -> list[pd.Timestamp]:
    return list(pd.date_range(start.normalize().replace(day=1), end, freq="MS"))


def _cache_path(*parts: str) -> str:
    p = os.path.join(CACHE_DIR, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


# ------------------------------------------------------------ Binance Vision archive
def probe_vision() -> None:
    """Быстрая проверка доступа к архиву, чтобы не ждать ретраев по каждому файлу."""
    try:
        requests.head("https://data.binance.vision/", timeout=15)
    except requests.RequestException as e:
        raise DataUnavailable(f"нет сетевого доступа к data.binance.vision ({type(e).__name__}: {e})") from e


def _vision_file(kind: str, sym: str, tf: str | None, period: str, stamp: str) -> pd.DataFrame | None:
    """Один файл архива (monthly/daily) с кэшем parquet. Возвращает None, если файла нет."""
    tf_part = f"{tf}/" if tf else ""
    tf_name = f"-{tf}" if tf else ""
    fname = f"{sym}{tf_name}-{stamp}" if kind != "metrics" else f"{sym}-metrics-{stamp}"
    if kind == "fundingRate":
        fname = f"{sym}-fundingRate-{stamp}"
    cache = _cache_path("vision", kind, sym, tf or "na", f"{stamp}.parquet")
    miss = cache + ".missing"
    if os.path.exists(cache):
        return pd.read_parquet(cache)
    if os.path.exists(miss):
        return None
    url = f"{VISION}/{period}/{kind}/{sym}/{tf_part}{fname}.zip"
    blob = _http_get(url)
    if blob is None:
        open(miss, "w").close()
        return None
    cols = KLINE_COLS if kind in ("klines", "premiumIndexKlines", "markPriceKlines", "indexPriceKlines") else None
    df = _read_zip_csv(blob, cols)
    df.to_parquet(cache)
    return df


def _vision_range(kind: str, sym: str, tf: str | None, start: pd.Timestamp, end: pd.Timestamp,
                  daily_only: bool = False, workers: int = 8) -> pd.DataFrame:
    """Полные месяцы — monthly-архивы; текущий (неполный) месяц — daily-архивы."""
    today = pd.Timestamp.now(tz="UTC").normalize()
    end = min(end, today)
    jobs: list[tuple[str, str]] = []
    if daily_only:
        jobs = [("daily", d.strftime("%Y-%m-%d")) for d in pd.date_range(start.normalize(), end - pd.Timedelta(days=1))]
    else:
        cur_month = today.replace(day=1)
        for m in _months(start, end):
            if m < cur_month:
                jobs.append(("monthly", m.strftime("%Y-%m")))
            else:
                jobs += [("daily", d.strftime("%Y-%m-%d"))
                         for d in pd.date_range(max(m, start.normalize()), end - pd.Timedelta(days=1))]
    with ThreadPoolExecutor(workers) as ex:
        parts = list(ex.map(lambda j: _vision_file(kind, sym, tf, j[0], j[1]), jobs))
    parts = [p for p in parts if p is not None and len(p)]
    if not parts:
        return pd.DataFrame()
    return pd.concat(parts, ignore_index=True)


def vision_klines(sym: str, tf: str, start, end, kind: str = "klines") -> pd.DataFrame:
    df = _vision_range(kind, sym, tf, _ts(start), _ts(end))
    if df.empty:
        return df
    df.index = _to_utc_index(df["open_time"])
    df = df[["open", "high", "low", "close", "volume", "quote_volume", "taker_buy_volume"]].astype(float)
    df = df[~df.index.duplicated(keep="last")].sort_index()
    return df.loc[_ts(start):_ts(end) - pd.Timedelta(microseconds=1)]


def vision_funding(sym: str, start, end) -> pd.Series:
    df = _vision_range("fundingRate", sym, None, _ts(start), _ts(end))
    if df.empty:
        return pd.Series(dtype=float, name="funding")
    idx = _to_utc_index(df["calc_time"]).floor("s")
    s = pd.Series(df["last_funding_rate"].astype(float).values, index=idx, name="funding")
    s = s[~s.index.duplicated(keep="last")].sort_index()
    return s.loc[_ts(start):_ts(end)]


def vision_metrics(sym: str, start, end) -> pd.DataFrame:
    """5-мин снимки: OI, OI в USD, L/S ratio (топ-трейдеры, аккаунты), taker L/S vol ratio."""
    df = _vision_range("metrics", sym, None, _ts(start), _ts(end), daily_only=True)
    if df.empty:
        return df
    df.index = _to_utc_index(df["create_time"])
    df = df.drop(columns=[c for c in ("create_time", "symbol") if c in df.columns]).astype(float)
    df = df.rename(columns={
        "sum_open_interest": "oi", "sum_open_interest_value": "oi_usd",
        "count_toptrader_long_short_ratio": "ls_top_accounts",
        "sum_toptrader_long_short_ratio": "ls_top_positions",
        "count_long_short_ratio": "ls_accounts",
        "sum_taker_long_short_vol_ratio": "taker_ls_vol"})
    return df[~df.index.duplicated(keep="last")].sort_index()


# ------------------------------------------------------------------------ ccxt path
def _ccxt_exchange(name: str):
    import ccxt  # локальный импорт: ccxt не обязателен для пути Binance Vision
    ex = getattr(ccxt, {"binance": "binanceusdm"}.get(name, name))({"enableRateLimit": True})
    try:
        ex.load_markets()
    except ccxt.BaseError as e:
        raise DataUnavailable(f"{name}: API недоступен ({type(e).__name__}: {str(e)[:200]})") from e
    return ex


def ccxt_ohlcv(exchange: str, symbol: str, tf: str, start, end, limit: int = 1000) -> pd.DataFrame:
    ex = _ccxt_exchange(exchange)
    if symbol not in ex.markets:
        raise DataUnavailable(
            f"{exchange}: рынок {symbol} отсутствует в load_markets() (снят с торгов?). "
            "Для истории снятых контрактов используйте exchange='binance' (архив data.binance.vision).")
    since, until = int(_ts(start).timestamp() * 1000), int(_ts(end).timestamp() * 1000)
    rows, step = [], pd.Timedelta(TF_PANDAS[tf]).value // 10**6
    while since < until:
        batch = ex.fetch_ohlcv(symbol, tf, since=since, limit=limit)
        if not batch:
            break
        rows += batch
        nxt = batch[-1][0] + step
        if nxt <= since:
            break
        since = nxt
    if not rows:
        return pd.DataFrame()
    df = pd.DataFrame(rows, columns=["t", "open", "high", "low", "close", "volume"])
    df.index = pd.to_datetime(df.pop("t"), unit="ms", utc=True)
    df = df[~df.index.duplicated(keep="last")].sort_index().astype(float)
    return df.loc[: _ts(end) - pd.Timedelta(microseconds=1)]


def ccxt_funding(exchange: str, symbol: str, start, end) -> pd.Series:
    ex = _ccxt_exchange(exchange)
    since, until, out = int(_ts(start).timestamp() * 1000), int(_ts(end).timestamp() * 1000), []
    while since < until:
        batch = ex.fetch_funding_rate_history(symbol, since=since, limit=200)
        if not batch:
            break
        out += batch
        nxt = batch[-1]["timestamp"] + 1
        if nxt <= since:
            break
        since = nxt
    s = pd.Series({pd.Timestamp(b["timestamp"], unit="ms", tz="UTC"): b["fundingRate"] for b in out},
                  name="funding", dtype=float)
    return s.sort_index()


def ccxt_open_interest(exchange: str, symbol: str, start, end, tf: str = "1h") -> pd.Series:
    ex = _ccxt_exchange(exchange)
    if not ex.has.get("fetchOpenInterestHistory"):
        raise DataUnavailable(f"{exchange}: нет fetchOpenInterestHistory в ccxt")
    since, until, out = int(_ts(start).timestamp() * 1000), int(_ts(end).timestamp() * 1000), []
    while since < until:
        batch = ex.fetch_open_interest_history(symbol, tf, since=since, limit=200)
        if not batch:
            break
        out += batch
        nxt = batch[-1]["timestamp"] + 1
        if nxt <= since:
            break
        since = nxt
    s = pd.Series({pd.Timestamp(b["timestamp"], unit="ms", tz="UTC"):
                   b.get("openInterestAmount") or b.get("openInterestValue") for b in out},
                  name="oi", dtype=float)
    return s.sort_index()


# ------------------------------------------------------------------ quality control
@dataclass
class QualityReport:
    n_bars: int
    expected_bars: int
    missing_bars: int
    gaps: list            # [(from, to, bars)]
    duplicates: int
    ohlc_violations: int
    outliers: list        # [(time, log_return, robust_z)]
    zero_volume_bars: int

    def as_dict(self) -> dict:
        return {"bars": self.n_bars, "expected": self.expected_bars, "missing": self.missing_bars,
                "gaps>1bar": len(self.gaps), "duplicates": self.duplicates,
                "ohlc_violations": self.ohlc_violations, "outliers(|z|>10 MAD)": len(self.outliers),
                "zero_volume": self.zero_volume_bars}


def quality_check(df: pd.DataFrame, tf: str, z_thr: float = 10.0) -> QualityReport:
    """Пропуски, дубли, нарушения OHLC, выбросы (робастный z-score по MAD лог-доходностей).
    Выбросы только ПОМЕЧАЮТСЯ, а не удаляются: в крипте резкие движения реальны."""
    step = pd.Timedelta(TF_PANDAS[tf])
    dup = int(df.index.duplicated().sum())
    d = df[~df.index.duplicated()]
    expected = int((d.index[-1] - d.index[0]) / step) + 1 if len(d) else 0
    dt = d.index.to_series().diff()
    gi = dt[dt > step]
    gaps = [(str(t - g), str(t), int(g / step) - 1) for t, g in gi.items()]
    viol = int(((d["high"] < d[["open", "close"]].max(axis=1) - 1e-12) |
                (d["low"] > d[["open", "close"]].min(axis=1) + 1e-12) | (d["low"] <= 0)).sum())
    r = np.log(d["close"]).diff().dropna()
    mad = (r - r.median()).abs().median() * 1.4826
    z = (r - r.median()) / (mad if mad > 0 else np.nan)
    out = [(str(t), float(r[t]), float(z[t])) for t in z[z.abs() > z_thr].index]
    return QualityReport(len(d), expected, expected - len(d), gaps, dup, viol, out,
                         int((d["volume"] <= 0).sum()))


# ------------------------------------------------------------------ assembled panel
def _segments(exchange: str, since, until):
    for seg in SYMBOL_HISTORY[exchange]:
        s = max(_ts(since), _ts(seg["start"])) if seg["start"] else _ts(since)
        e = min(_ts(until), _ts(seg["end"])) if seg["end"] else _ts(until)
        if s < e:
            yield seg, s, e


def load_perp(exchange: str, tf: str, since, until=None) -> pd.DataFrame:
    """OHLCV перпетуала TON->GRAM, склеенный по SYMBOL_HISTORY.
    Колонки: open high low close volume [quote_volume taker_buy_volume] symbol gap_before.
    gap_before=True означает, что перед баром был разрыв (> 1 бара) — через него
    позицию держать нельзя (контракт был рассчитан/снят)."""
    until = until or pd.Timestamp.now(tz="UTC").floor(TF_PANDAS[tf])
    parts = []
    if exchange == "binance":
        probe_vision()
    for seg, s, e in _segments(exchange, since, until):
        if exchange == "binance":
            df = vision_klines(seg["raw"], tf, s, e)
        else:
            try:
                df = ccxt_ohlcv(exchange, seg["ccxt"], tf, s, e)
            except DataUnavailable as err:
                print(f"[data] пропуск сегмента {seg['ccxt']}: {err}")
                continue
        if len(df):
            df["symbol"] = seg["raw"]
            parts.append(df)
    if not parts:
        raise DataUnavailable(f"{exchange}: нет OHLCV ни для одного сегмента {SYMBOL_HISTORY[exchange]}")
    out = pd.concat(parts).sort_index()
    out = out[~out.index.duplicated(keep="last")]
    step = pd.Timedelta(TF_PANDAS[tf])
    out["gap_before"] = out.index.to_series().diff().gt(step).values
    return out


def load_funding(exchange: str, since, until=None) -> pd.Series:
    until = until or pd.Timestamp.now(tz="UTC")
    parts = []
    for seg, s, e in _segments(exchange, since, until):
        try:
            f = vision_funding(seg["raw"], s, e) if exchange == "binance" else ccxt_funding(exchange, seg["ccxt"], s, e)
            parts.append(f)
        except Exception as err:  # noqa: BLE001 — сообщаем и продолжаем
            print(f"[data] funding {seg['raw']}: {err}")
    return pd.concat(parts).sort_index() if parts else pd.Series(dtype=float, name="funding")


def load_metrics(exchange: str, since, until=None) -> pd.DataFrame:
    until = until or pd.Timestamp.now(tz="UTC")
    parts = []
    for seg, s, e in _segments(exchange, since, until):
        try:
            if exchange == "binance":
                parts.append(vision_metrics(seg["raw"], s, e))
            else:
                parts.append(ccxt_open_interest(exchange, seg["ccxt"], s, e).to_frame())
        except Exception as err:  # noqa: BLE001
            print(f"[data] OI/metrics {seg['raw']}: {err}")
    parts = [p for p in parts if len(p)]
    return pd.concat(parts).sort_index() if parts else pd.DataFrame()


def load_premium(exchange: str, tf: str, since, until=None) -> pd.Series:
    """Premium index (perp vs index) = basis. Только Binance-архив."""
    if exchange != "binance":
        return pd.Series(dtype=float, name="premium")
    until = until or pd.Timestamp.now(tz="UTC")
    parts = [vision_klines(seg["raw"], tf, s, e, kind="premiumIndexKlines")["close"]
             for seg, s, e in _segments(exchange, since, until)]
    parts = [p for p in parts if len(p)]
    return pd.concat(parts).sort_index().rename("premium") if parts else pd.Series(dtype=float, name="premium")


def load_bench(exchange: str, asset: str, tf: str, since, until=None) -> pd.DataFrame:
    until = until or pd.Timestamp.now(tz="UTC").floor(TF_PANDAS[tf])
    sym = BENCH_SYMBOLS[exchange][asset]
    return vision_klines(sym, tf, since, until) if exchange == "binance" else ccxt_ohlcv(exchange, sym, tf, since, until)


def build_panel(exchange: str = "binance", tf: str = "1h", since: str = "2022-01-01", until=None) -> dict:
    """Всё, что нужно исследованию, одним словарём + отчёт о качестве."""
    perp = load_perp(exchange, tf, since, until)
    start = perp.index[0]
    panel = {
        "perp": perp,
        "funding": load_funding(exchange, start, until),
        "metrics": load_metrics(exchange, start, until),
        "premium": load_premium(exchange, tf, start, until),
        "btc": load_bench(exchange, "BTC", tf, start, until),
        "eth": load_bench(exchange, "ETH", tf, start, until),
        "tf": tf, "exchange": exchange,
    }
    panel["quality"] = quality_check(perp, tf)
    return panel


def resample_ohlcv(df: pd.DataFrame, tf: str) -> pd.DataFrame:
    """Агрегация в старший ТФ (индекс — время открытия, неполный последний бар отбрасывается)."""
    rule = TF_PANDAS[tf]
    agg = {"open": "first", "high": "max", "low": "min", "close": "last", "volume": "sum"}
    for c in ("quote_volume", "taker_buy_volume"):
        if c in df.columns:
            agg[c] = "sum"
    out = df.resample(rule, label="left", closed="left").agg(agg).dropna(subset=["close"])
    counts = df["close"].resample(rule, label="left", closed="left").count()
    base = pd.Timedelta(rule) / (df.index.to_series().diff().median())
    return out[counts.reindex(out.index) >= int(round(base))]
