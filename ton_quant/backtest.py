"""
backtest.py — событийный бэктест перпетуала с реалистичными издержками.

Правила исполнения (без look-ahead):
  * сигнал на закрытии бара t -> рыночный ордер по OPEN бара t+1 (+ проскальзывание, taker);
  * стоп/тейк/ликвидация проверяются внутри бара по high/low; если в одном баре
    задеты и стоп, и тейк — считаем, что сработал СТОП (консервативно);
  * гэп через стоп — исполнение по open (хуже стопа);
  * funding: списывается/начисляется, если позиция открыта в момент расчёта
    (метка funding в интервале (open, close] бара), по номиналу на close;
  * изолированная маржа, цена ликвидации: long  entry*(1-1/L+mmr), short entry*(1+1/L-mmr);
  * через разрыв данных (снятие TONUSDT / листинг GRAMUSDT) позицию не держим:
    закрываем по close последнего бара перед разрывом (дата делистинга объявлялась заранее).
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from config import CostModel, RiskModel, TF_PANDAS


@dataclass
class BTResult:
    equity: pd.Series
    trades: pd.DataFrame
    position: pd.Series            # -1/0/+1 по барам
    costs: dict = field(default_factory=dict)


def funding_per_bar(idx: pd.DatetimeIndex, tf: str, funding: pd.Series | None) -> np.ndarray:
    """Сумма ставок funding с метками в (open, close] каждого бара."""
    out = np.zeros(len(idx))
    if funding is None or not len(funding):
        return out
    close_t = idx + pd.Timedelta(TF_PANDAS[tf])
    pos = np.searchsorted(close_t.values, funding.index.values, side="left")  # первый бар с close >= t_f
    ok = (pos < len(idx))
    ok &= funding.index.values > idx.values[np.minimum(pos, len(idx) - 1)]  # t_f > open бара
    np.add.at(out, pos[ok], funding.values[ok])
    return out


def run_backtest(df: pd.DataFrame, sig: pd.DataFrame, atr: pd.Series, tf: str,
                 funding: pd.Series | None = None, costs: CostModel | None = None,
                 risk: RiskModel | None = None, regime: pd.Series | None = None,
                 fixed_notional_frac: float | None = None, allow_reverse: bool = True) -> BTResult:
    costs, risk = costs or CostModel(), risk or RiskModel()
    o, h, l, c = (df[k].to_numpy(float) for k in ("open", "high", "low", "close"))
    n = len(df)
    idx = df.index
    gap_next = np.r_[df["gap_before"].to_numpy(bool)[1:], False] if "gap_before" in df else np.zeros(n, bool)
    a = atr.reindex(idx).to_numpy(float)
    fund = funding_per_bar(idx, tf, funding)
    le, se = sig["long_entry"].to_numpy(bool), sig["short_entry"].to_numpy(bool)
    lx, sx = sig["long_exit"].to_numpy(bool), sig["short_exit"].to_numpy(bool)
    stop_m = np.broadcast_to(np.asarray(sig["stop_atr"], float), (n,))
    tp_m = np.broadcast_to(np.asarray(sig["tp_atr"], float), (n,))
    mh = np.broadcast_to(np.asarray(sig["max_hold"], float), (n,))
    sm = np.broadcast_to(np.asarray(sig["size_mult"], float), (n,))
    conf = sig["confluence"].to_numpy(float) if "confluence" in sig else np.zeros(n)
    reg = regime.reindex(idx).astype(object).to_numpy() if regime is not None else np.array([None] * n)
    hours = idx.hour.to_numpy()
    lev, mmr = risk.leverage, risk.maint_margin_rate

    def slip(i):
        m = costs.thin_hours_mult if hours[i] in costs.thin_hours else 1.0
        return costs.slippage_bps * 1e-4 * m

    cash = risk.initial_capital         # реализованный капитал
    side = 0; qty = entry = stop = tp = liq = 0.0; margin = 0.0
    ent_i = -1; ent_fee = ent_slip = fund_acc = 0.0; risk_amt = 0.0
    pending = None                      # ("open", side, signal_bar) | ("close",) | ("reverse", side, signal_bar)
    eq = np.empty(n); pos_arr = np.zeros(n)
    trades = []
    tot = {"fees": 0.0, "slippage": 0.0, "funding": 0.0, "liquidations": 0}

    def close_pos(i, px, reason, fee_rate, slip_rate):
        nonlocal cash, side, qty, fund_acc
        fill = px * (1 - side * slip_rate)
        gross = side * qty * (fill - entry)
        fee = qty * fill * fee_rate
        s_cost = qty * px * slip_rate
        if reason == "liquidation":
            # изолированная маржа теряется целиком (клиринговый сбор биржи покрывается из неё же)
            gross, fee, s_cost = -margin, 0.0, 0.0
            tot["liquidations"] += 1
        net = gross - fee - ent_fee + fund_acc
        cash += gross - fee
        tot["fees"] += fee; tot["slippage"] += s_cost
        trades.append({"entry_time": idx[ent_i], "exit_time": idx[i], "side": side, "entry": entry,
                       "exit": fill, "qty": qty, "gross": gross,  # gross уже включает проскальзывание (цены fill)
                       "fees": fee + ent_fee, "slippage": ent_slip + s_cost, "funding": fund_acc,
                       "net": net, "R": net / risk_amt if risk_amt > 0 else np.nan, "bars": i - ent_i + 1,
                       "reason": reason, "regime": reg[ent_i], "confluence": conf[max(ent_i - 1, 0)]})
        side = 0; qty = 0.0; fund_acc = 0.0

    def open_pos(i, new_side, sbar):
        nonlocal cash, side, qty, entry, stop, tp, liq, margin, ent_i, ent_fee, ent_slip, risk_amt
        if not np.isfinite(a[sbar]) or sm[sbar] <= 0:
            return
        sr = slip(i)
        fill = o[i] * (1 + new_side * sr)
        eq_now = cash
        if fixed_notional_frac is not None:
            q = eq_now * fixed_notional_frac / fill
            dist = np.nan
        else:
            dist = stop_m[sbar] * a[sbar]
            if not np.isfinite(dist) or dist <= 0:
                return
            q = eq_now * risk.risk_per_trade * sm[sbar] / dist
        q = min(q, eq_now * lev / fill)            # ограничение номинала плечом
        if q <= 0:
            return
        side, qty, entry, ent_i = new_side, q, fill, i
        margin = q * fill / lev
        stop = fill - side * dist if np.isfinite(dist) else np.nan
        tp = fill + side * tp_m[sbar] * a[sbar] if np.isfinite(tp_m[sbar]) else np.nan
        liq = fill * (1 - side * (1 / lev - mmr))
        risk_amt = q * dist if np.isfinite(dist) else q * fill
        ent_fee = q * fill * costs.taker_fee
        ent_slip = q * o[i] * sr
        cash -= ent_fee
        tot["fees"] += ent_fee; tot["slippage"] += ent_slip

    for i in range(n):
        # 1) исполнение отложенных ордеров по open[i]
        if pending is not None:
            kind = pending[0]
            if kind in ("close", "reverse") and side != 0:
                close_pos(i, o[i], "signal" if kind == "close" else "reverse", costs.taker_fee, slip(i))
            if kind in ("open", "reverse") and side == 0:
                open_pos(i, pending[1], pending[2])
            pending = None
        # 2) внутрибарные стоп/тейк/ликвидация
        if side != 0:
            adverse = l[i] if side > 0 else h[i]
            favor = h[i] if side > 0 else l[i]
            hit_liq = (adverse - liq) * side <= 0
            hit_stop = np.isfinite(stop) and (adverse - stop) * side <= 0
            liq_first = hit_liq and (not np.isfinite(stop) or (liq - stop) * side >= 0)
            if liq_first:
                close_pos(i, liq, "liquidation", 0.0, 0.0)
            elif hit_stop:
                px = o[i] if (o[i] - stop) * side < 0 else stop   # гэп через стоп
                close_pos(i, px, "stop", costs.taker_fee, slip(i))
            elif np.isfinite(tp) and (favor - tp) * side >= 0:
                px = o[i] if (o[i] - tp) * side > 0 else tp
                fr = costs.maker_fee if costs.tp_is_maker else costs.taker_fee
                close_pos(i, px, "take_profit", fr, 0.0 if costs.tp_is_maker else slip(i))
        # 3) funding по позиции, открытой на момент расчёта
        if side != 0 and fund[i] != 0.0:
            pay = -side * qty * c[i] * fund[i]
            fund_acc += pay; cash += pay; tot["funding"] += pay
        # 4) принудительное закрытие перед разрывом данных/делистингом
        if side != 0 and gap_next[i]:
            close_pos(i, c[i], "delisting_gap", costs.taker_fee, slip(i))
        # 5) mark-to-market
        eq[i] = cash + (side * qty * (c[i] - entry) if side != 0 else 0.0)
        pos_arr[i] = side
        if eq[i] <= 0:
            eq[i:] = 0.0
            break
        # 6) сигналы на закрытии бара i -> ордер на open[i+1]
        if i == n - 1 or gap_next[i]:
            continue
        if side != 0:
            held = i - ent_i + 1
            want_exit = (lx[i] if side > 0 else sx[i]) or held >= mh[i]
            opp = se[i] if side > 0 else le[i]
            if opp and allow_reverse and sm[i] > 0:
                pending = ("reverse", -side, i)
            elif want_exit or opp:
                pending = ("close",)
        else:
            if le[i] and not se[i]:
                pending = ("open", 1, i)
            elif se[i] and not le[i]:
                pending = ("open", -1, i)
    if side != 0:  # закрываем по последнему close для учёта
        close_pos(n - 1, c[n - 1], "end", costs.taker_fee, slip(n - 1))
        eq[n - 1] = cash
    tr = pd.DataFrame(trades)
    return BTResult(pd.Series(eq, idx, name="equity"), tr, pd.Series(pos_arr, idx, name="position"), tot)


def buy_and_hold(df, tf, funding=None, costs=None, risk=None) -> BTResult:
    """Бенчмарк: лонг перпетуала 1x (с funding и комиссиями, переоткрытие после разрыва)."""
    n = len(df)
    sig = pd.DataFrame({"long_entry": np.ones(n, bool), "short_entry": np.zeros(n, bool),
                        "long_exit": np.zeros(n, bool), "short_exit": np.zeros(n, bool),
                        "stop_atr": np.nan, "tp_atr": np.nan, "max_hold": 10**9, "size_mult": 1.0},
                       index=df.index)
    r = risk or RiskModel()
    r = RiskModel(**{**r.__dict__, "leverage": 1.0})
    return run_backtest(df, sig, pd.Series(1.0, df.index), tf, funding, costs, r, fixed_notional_frac=1.0)


# ------------------------------------------------------------------- метрики
def daily_returns(equity: pd.Series) -> pd.Series:
    """Дневные доходности; после разорения (капитал 0) ряд обрывается на -100%."""
    d = equity.resample("1D").last().ffill()
    ruin = np.flatnonzero(d.to_numpy() <= 0)
    if len(ruin):
        d = d.iloc[:ruin[0] + 1]
    return d.pct_change().replace([np.inf, -np.inf], np.nan).dropna()


def max_streak(x: np.ndarray) -> int:
    best = cur = 0
    for v in x:
        cur = cur + 1 if v else 0
        best = max(best, cur)
    return best


def metrics(res: BTResult, freq_per_year: int = 365) -> dict:
    eq = res.equity
    eq = eq[eq > 0] if (eq > 0).any() else eq
    r = daily_returns(res.equity)
    years = max((eq.index[-1] - eq.index[0]).days / 365.25, 1e-9)
    total = eq.iloc[-1] / eq.iloc[0] - 1 if len(eq) else np.nan
    cagr = (1 + total) ** (1 / years) - 1 if total > -1 else -1.0
    sd = r.std()
    sharpe = r.mean() / sd * np.sqrt(freq_per_year) if sd > 0 else np.nan
    dn = r[r < 0].std()
    sortino = r.mean() / dn * np.sqrt(freq_per_year) if dn and dn > 0 else np.nan
    dd = eq / eq.cummax() - 1
    mdd = dd.min()
    t = res.trades
    out = {"CAGR": cagr, "Sharpe": sharpe, "Sortino": sortino,
           "Calmar": cagr / abs(mdd) if mdd < 0 else np.nan, "MaxDD": mdd, "TotalReturn": total,
           "Trades": len(t), "TimeInMarket": float((res.position != 0).mean())}
    if len(t):
        wins, losses = t.loc[t.net > 0, "net"], t.loc[t.net <= 0, "net"]
        out.update({
            "WinRate": len(wins) / len(t),
            "ProfitFactor": wins.sum() / abs(losses.sum()) if len(losses) and losses.sum() != 0 else np.nan,
            "AvgTrade$": t.net.mean(), "AvgTrade%": (t.net / (t.qty * t.entry)).mean(),
            "Expectancy_R": t.R.mean(), "MaxLosingStreak": max_streak((t.net <= 0).to_numpy()),
            "AvgBars": t.bars.mean(), "Liquidations": res.costs.get("liquidations", 0),
            "Fees$": t.fees.sum(), "Slippage$": t.slippage.sum(), "Funding$": t.funding.sum(),
            "Gross$": (t.net + t.fees - t.funding).sum(), "Net$": t.net.sum(),
        })
        # значимость средней сделки (t-stat по R); ниже ~30 сделок выводы ненадёжны
        out["tstat_R"] = t.R.mean() / (t.R.std(ddof=1) / np.sqrt(len(t))) if len(t) > 2 and t.R.std() > 0 else np.nan
    return out


def metrics_by(res: BTResult, key: str) -> pd.DataFrame:
    """Разбивка метрик сделок по режиму на входе ('regime') или по году ('year')."""
    t = res.trades.copy()
    if not len(t):
        return pd.DataFrame()
    if key == "year":
        t["year"] = pd.to_datetime(t["exit_time"]).dt.year
    rows = {}
    for k, g in t.groupby(key):
        wins, losses = g.net[g.net > 0], g.net[g.net <= 0]
        rows[k] = {"Trades": len(g), "WinRate": (g.net > 0).mean(), "Net$": g.net.sum(),
                   "ProfitFactor": wins.sum() / abs(losses.sum()) if losses.sum() != 0 else np.nan,
                   "Expectancy_R": g.R.mean(), "Fees$": g.fees.sum(), "Funding$": g.funding.sum()}
    if key == "year":
        yr = res.equity.resample("YE").last()
        prev = yr.shift(1)
        prev.iloc[0] = res.equity.iloc[0]
        for y, v in (yr / prev - 1).items():
            rows.setdefault(y.year, {})["EquityReturn"] = v
    return pd.DataFrame(rows).T.sort_index()
