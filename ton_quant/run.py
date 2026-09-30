"""
run.py — полный исследовательский прогон.

  python run.py                               # Binance (архив data.binance.vision), 1h, с 2022-01-01
  python run.py --exchange okx                # через ccxt (история снятых TON-контрактов может быть недоступна)
  python run.py --synthetic null              # самопроверка фреймворка на случайном блуждании
  python run.py --synthetic planted           # самопроверка мощности (вшитый эффект funding)
  python run.py --fast                        # меньше симуляций (для отладки)

Отчёт: reports/<exchange>/report.md + PNG.
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

warnings.filterwarnings("ignore", category=RuntimeWarning)
warnings.filterwarnings("ignore", category=FutureWarning)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import research as rs  # noqa: E402
from backtest import buy_and_hold, daily_returns, metrics, metrics_by, run_backtest  # noqa: E402
from config import REPORT_DIR, ResearchConfig, set_seed  # noqa: E402
from data import DataUnavailable, build_panel, load_bench, load_perp, resample_ohlcv  # noqa: E402
from features import build_features  # noqa: E402
from regime import fit_predict_regime  # noqa: E402
from report import (Report, plot_bars, plot_car, plot_drawdown, plot_equity, plot_heatmap,  # noqa: E402
                    plot_hist, plot_hours, plot_regimes)
from signals import STRATEGIES, generate, param_grid  # noqa: E402
from validation import (deflated_sharpe, monte_carlo, random_entries, sensitivity_summary,  # noqa: E402
                        sharpe_ci, walk_forward, whites_reality_check)


def wilson(k: int, n: int, z: float = 1.645) -> tuple[float, float]:
    if n == 0:
        return (np.nan, np.nan)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    w = z * np.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (c - w, c + w)


def edge_verdict(m: dict, dsr: dict, rc_p: float, rnd: dict, mc: dict) -> tuple[bool, list[str]]:
    """Критерии заявлены ДО прогона. Edge признаётся только если выполнены ВСЕ."""
    checks = [
        ("OOS сделок >= 30", m.get("Trades", 0) >= 30),
        ("OOS Sharpe > 0.5 после издержек", (m.get("Sharpe") or -9) > 0.5),
        ("Deflated Sharpe > 0.95", (dsr.get("DSR") or 0) > 0.95),
        ("White's RC p < 0.05", np.isfinite(rc_p) and rc_p < 0.05),
        ("Случайные входы p < 0.05", (rnd.get("p_value(random>=strategy)", 1.0)) < 0.05),
        ("MC P(убыток) < 20%", (mc.get("P(total_ret<0)", 1.0)) < 0.2),
    ]
    return all(ok for _, ok in checks), [f"{'✔' if ok else '✘'} {name}" for name, ok in checks]


def current_signal(name: str, wf, f, df, cfg: ResearchConfig) -> dict:
    """Сигнал на последнем закрытом баре по параметрам ПОСЛЕДНЕГО фолда walk-forward."""
    last = wf.folds.dropna(subset=["params"])
    if not len(last):
        return {"status": "нет параметров (во всех фолдах стратегия была вне рынка)"}
    params = last.iloc[-1]["params"]
    n = len(df)
    tr_idx = df.index[max(0, n - cfg.wf.train_bars):]
    regime = fit_predict_regime(f, tr_idx, "hmm")
    sig = generate(name, f, df["close"], regime, params)
    win = slice(max(0, n - cfg.wf.test_bars), n)
    bt = run_backtest(df.iloc[win], sig.iloc[win], f["atr"], cfg.base_tf, None, cfg.costs, cfg.risk, regime)
    t_last = df.index[-1]
    side = 0
    if sig["long_entry"].iloc[-1]:
        side = 1
    elif sig["short_entry"].iloc[-1]:
        side = -1
    open_pos = len(bt.trades) and bt.trades.iloc[-1]["reason"] == "end"
    out = {"bar": str(t_last), "params": params, "regime_now": regime.iloc[-1],
           "confluence_now": float(sig["confluence"].iloc[-1]),
           "new_entry_signal": {1: "LONG", -1: "SHORT", 0: "нет"}[side],
           "strategy_in_position_now": bool(open_pos)}
    if open_pos:
        tr = bt.trades.iloc[-1]
        out["open_position"] = {"side": "LONG" if tr.side > 0 else "SHORT", "entry": float(tr.entry),
                                "entry_time": str(tr.entry_time)}
    if side != 0:
        px, a = float(df["close"].iloc[-1]), float(f["atr"].iloc[-1])
        stop_d = float(sig["stop_atr"].iloc[-1]) * a
        eq = cfg.risk.initial_capital
        qty = min(eq * cfg.risk.risk_per_trade / stop_d, eq * cfg.risk.leverage / px)
        out.update({"entry_ref(close; исполнение по след. open)": px,
                    "stop": px - side * stop_d, "target_1R": px + side * stop_d,
                    "target_2R": px + side * 2 * stop_d,
                    "qty_per_10k_capital": qty, "notional": qty * px,
                    "risk_$_per_10k": qty * stop_d,
                    "liq_price(isolated)": px * (1 - side * (1 / cfg.risk.leverage - cfg.risk.maint_margin_rate))})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--exchange", default="binance", choices=["binance", "bybit", "okx"])
    ap.add_argument("--since", default="2022-01-01")
    ap.add_argument("--tf", default="1h")
    ap.add_argument("--synthetic", choices=["null", "planted"], default=None)
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--strategies", nargs="*", default=list(STRATEGIES))
    ap.add_argument("--skip-ml", action="store_true")
    args = ap.parse_args(argv)

    set_seed()
    cfg = ResearchConfig(exchange=args.exchange, base_tf=args.tf, since=args.since)
    if args.fast:
        cfg.mc_sims, cfg.random_entry_sims = 300, 40
    t0 = time.time()

    # ------------------------------------------------------------------ данные
    if args.synthetic:
        from synthetic import make_panel
        panel = make_panel(mode=args.synthetic)
        tag = f"synthetic_{args.synthetic}"
    else:
        try:
            panel = build_panel(args.exchange, args.tf, args.since)
        except DataUnavailable as e:
            print(f"\n[ОСТАНОВ] Данные недоступны: {e}\n"
                  "Что нужно: сетевой доступ к data.binance.vision (или REST API биржи для ccxt). "
                  "Данные НЕ подменяются синтетикой.")
            return 2
        tag = args.exchange
    out_dir = os.path.join(REPORT_DIR, tag)
    rep = Report(out_dir, f"TON/GRAM perpetual — исследовательский отчёт ({tag})")
    if args.synthetic:
        rep.p("> **ВНИМАНИЕ: СИНТЕТИЧЕСКИЕ ДАННЫЕ.** Этот отчёт проверяет сам фреймворк "
              "(контроль ложноположительных/мощности). Цифры НЕ относятся к рынку TON.")
    df = panel["perp"]
    rep.h("0. Данные и качество")
    rep.p(f"Биржа: {panel['exchange']}, ТФ {args.tf}, период {df.index[0]} — {df.index[-1]}, "
          f"сегменты: {', '.join(df['symbol'].unique())}. Funding: {len(panel['funding'])} точек, "
          f"OI/metrics: {len(panel['metrics'])} снимков.")
    rep.table(panel["quality"].as_dict())
    if panel["quality"].gaps:
        rep.p("Разрывы (>1 бара): " + "; ".join(f"{a} → {b} ({k} бар.)" for a, b, k in panel["quality"].gaps[:10]))

    print("[features] ...")
    f = build_features(panel, args.tf)
    funding = panel["funding"]

    # ---------------------------------------------------- гипотеза H1 и уровни
    daily = resample_ohlcv(df, "1d")
    btc_d = resample_ohlcv(panel["btc"], "1d") if len(panel["btc"]) else None
    h1 = rs.trend_hypothesis(daily, btc_d)
    rep.h("1. Гипотеза H1 «нейтрально-положительный тренд»")
    rep.p("Операционализация (заявлена до теста): подтверждена, если наклон лог-цены за 90 дней > 0 "
          "с HAC t > 1 **и** цена выше SMA200; опровергнута, если наклон < 0 с t < −1 **и** цена ниже SMA200.")
    rep.table({k: (str(v) if isinstance(v, tuple) else v) for k, v in h1.items()})
    rep.p("Ключевые уровни (узлы объёмного профиля за 180 дн. + экстремумы 20/60 дн.):")
    rep.table(rs.support_resistance(daily), "{:.4f}")

    # ------------------------------------------------ стратегии: walk-forward
    rep.h("2. Стратегии: walk-forward OOS (параметры выбраны только на train)")
    rep.p(f"Окна: train {cfg.wf.train_bars} бар., test {cfg.wf.test_bars}, шаг {cfg.wf.step_bars}. "
          f"Издержки: taker {cfg.costs.taker_fee:.4%}, maker {cfg.costs.maker_fee:.4%}, "
          f"проскальзывание {cfg.costs.slippage_bps} bps, funding — фактический. Риск/сделку "
          f"{cfg.risk.risk_per_trade:.1%}, плечо ≤ {cfg.risk.leverage}x, MMR {cfg.risk.maint_margin_rate:.1%}.")
    wfs, rows, all_cfg_rets = {}, {}, []
    for name in args.strategies:
        print(f"[walk-forward] {name}")
        wfs[name] = walk_forward(name, f, df, funding, cfg)
        all_cfg_rets.append(wfs[name].config_oos_returns)
    n_trials = sum(len(param_grid(n)) for n in args.strategies)
    R_all = pd.concat(all_cfg_rets, axis=1)
    trial_sr = (R_all.mean() / R_all.std()).to_numpy()

    # бенчмарк на том же OOS-отрезке
    any_wf = next(iter(wfs.values()))
    oos_start = any_wf.oos.equity.index[0]
    bh = buy_and_hold(df.loc[oos_start:], args.tf, funding, cfg.costs, cfg.risk)
    bh_ret = daily_returns(bh.equity)
    curves = {"Buy&Hold perp 1x": bh.equity}
    summary = {"Buy&Hold perp 1x": metrics(bh)}
    verdicts, validations = {}, {}
    for name, wf in wfs.items():
        m = metrics(wf.oos)
        summary[name] = m
        curves[name] = wf.oos.equity
        r = daily_returns(wf.oos.equity)
        dsr = deflated_sharpe(r, trial_sr)
        rc_cash = whites_reality_check(R_all, None, 500 if args.fast else 2000, cfg.bootstrap_block)
        rc_bh = whites_reality_check(R_all, bh_ret, 500 if args.fast else 2000, cfg.bootstrap_block)
        mc = monte_carlo(wf.oos, cfg.mc_sims)
        ci = sharpe_ci(r, 500 if args.fast else 2000, cfg.bootstrap_block)
        rnd = (random_entries(df.loc[oos_start:], f.loc[oos_start:], funding, cfg, wf.oos, cfg.random_entry_sims)
               if m["Trades"] >= 5 else {"note": "мало сделок"})
        ok, checks = edge_verdict(m, dsr, rc_cash.get("p_value", np.nan), rnd, mc)
        verdicts[name] = (ok, checks)
        validations[name] = {"dsr": dsr, "rc_cash": rc_cash, "rc_bh": rc_bh, "mc": mc, "ci": ci, "rnd": rnd}
    order = ["CAGR", "Sharpe", "Sortino", "Calmar", "MaxDD", "ProfitFactor", "WinRate", "AvgTrade%",
             "Expectancy_R", "tstat_R", "Trades", "MaxLosingStreak", "TimeInMarket", "Gross$", "Fees$",
             "Slippage$", "Funding$", "Net$", "Liquidations"]
    tab = pd.DataFrame(summary).T.reindex(columns=order)
    rep.table(tab)
    rep.p("*Gross$ — PnL по ценам исполнения (уже с проскальзыванием); Fees$ и Funding$ показывают, "
          "сколько «съели» комиссии и funding (Funding$ < 0 — уплачено).* ")
    rep.img(plot_equity(curves, rep.path("equity.png"), "OOS-капитал стратегий vs Buy&Hold"), "equity")
    rep.img(plot_drawdown({k: v for k, v in curves.items()}, rep.path("drawdown.png")), "drawdown")

    rep.h("2.1 Фолды walk-forward", 3)
    for name, wf in wfs.items():
        rep.p(f"**{name}**")
        fd = wf.folds.copy()
        fd["params"] = fd["params"].astype(str)
        rep.table(fd.set_index("fold"))

    rep.h("2.2 Разбивка по режимам и годам (OOS)", 3)
    for name, wf in wfs.items():
        if len(wf.oos.trades):
            rep.p(f"**{name}** — по режиму на входе:")
            rep.table(metrics_by(wf.oos, "regime"))
            rep.p(f"**{name}** — по годам:")
            rep.table(metrics_by(wf.oos, "year"))

    # ------------------------------------------------------------- валидация
    rep.h("3. Устойчивость и значимость")
    rep.p(f"Всего испытано конфигураций: **{n_trials}** (учтено в DSR и White's Reality Check).")
    vt = {}
    for name, v in validations.items():
        vt[name] = {"Sharpe_OOS": v["ci"].get("sharpe"), "Sharpe_CI90_lo": v["ci"].get("ci05"),
                    "Sharpe_CI90_hi": v["ci"].get("ci95"), "PSR(>0)": v["dsr"].get("PSR(>0)"),
                    "DSR": v["dsr"].get("DSR"), "DSR(var=1/T)": v["dsr"].get("DSR_nullvar"),
                    "SR0_ann(порог)": v["dsr"].get("SR0_ann"),
                    "RC_p(vs cash)": v["rc_cash"].get("p_value"), "RC_p(vs B&H)": v["rc_bh"].get("p_value"),
                    "MC_P(loss)": v["mc"].get("P(total_ret<0)"), "MC_ret_p05": v["mc"].get("boot_total_ret_p05"),
                    "MC_ret_p95": v["mc"].get("boot_total_ret_p95"), "MC_maxDD_p05": v["mc"].get("boot_maxdd_p05"),
                    "Random_p": v["rnd"].get("p_value(random>=strategy)"),
                    "Random_Sharpe_p50": v["rnd"].get("random_sharpe_p50")}
    rep.table(pd.DataFrame(vt).T)
    rep.p("Критерии edge (все обязательны):")
    for name, (ok, checks) in verdicts.items():
        rep.p(f"- **{name}: {'EDGE ЕСТЬ' if ok else 'edge не найден'}** — " + "; ".join(checks))
    best_name = max(wfs, key=lambda k: (summary[k].get("Sharpe") or -9) if np.isfinite(summary[k].get("Sharpe") or np.nan) else -9)
    mc_b = validations[best_name]["mc"]
    if "n_trades" in mc_b and mc_b.get("n_trades", 0) >= 5:
        from validation import trade_returns
        tr = trade_returns(wfs[best_name].oos)
        rng = np.random.default_rng(42)
        sims = [np.prod(1 + rng.choice(tr, len(tr))) - 1 for _ in range(cfg.mc_sims)]
        rep.img(plot_hist(np.array(sims), float(np.prod(1 + tr) - 1), rep.path("mc.png"),
                          f"Monte Carlo (bootstrap сделок): {best_name}", "итоговая доходность"), "mc")

    rep.h("3.1 Чувствительность параметров (карта устойчивости)", 3)
    for name, wf in wfs.items():
        ss = sensitivity_summary(wf)
        rep.p(f"**{name}**: доля конфигураций с медианным train-Sharpe > 0: "
              f"{(ss['median_train_sharpe'] > 0).mean():.0%}; с OOS-Sharpe > 0: {(ss['median_oos_sharpe'] > 0).mean():.0%}")
        rep.table(ss)
    if "trend" in wfs:
        ss = sensitivity_summary(wfs["trend"])
        hm = ss.pivot_table(index="adx_thr", columns="stop_atr", values="median_train_sharpe", aggfunc="median")
        rep.img(plot_heatmap(hm, rep.path("sens_trend.png"), "trend: медианный Sharpe (train) по сетке"), "sens")

    # -------------------------------------------------------------- режимы
    regime_all = fit_predict_regime(f, df.index[:cfg.wf.train_bars], "hmm")
    rep.img(plot_regimes(df["close"], regime_all, rep.path("regimes.png")), "regimes")
    rep.p("Режимы (HMM обучен на первом train-окне, дальше — каузальная фильтрация): "
          + ", ".join(f"{k}: {v:.0%}" for k, v in regime_all.value_counts(normalize=True).items()))

    # ------------------------------------------------ этап 4: исследования
    rep.h("4. Нестандартные подходы (вердикт только по OOS)")
    rows = []
    rows += rs.test_funding_contrarian(f, df["close"], cfg)
    rows += rs.test_liquidity_magnet(df, f, cfg)
    rep.p("**Funding как контрарный индикатор / «магниты» ликвидаций (прокси по OI):**")
    rep.table(pd.DataFrame(rows).set_index("hypothesis"))
    rep.p("**OI + цена + объём (средняя доходность через 24 бара по квадрантам):**")
    rep.table(rs.test_oi_price(f, df["close"], cfg).set_index("quadrant"), "{:.2f}")
    rep.p("**Очистка шума: наклон EMA vs Калман vs вейвлет (IC со знаком будущей доходности 24 бара):**")
    rep.table(rs.test_denoising(df["close"], cfg).set_index("hypothesis"))

    print("[lead-lag] ...")
    try:
        if args.synthetic:
            ton15, btc15 = df, panel["btc"]
            rep.p("_Lead-lag на синтетике считается на 1h (15m не генерируется)._")
        else:
            ton15 = load_perp(args.exchange, "15m", df.index[0])
            btc15 = load_bench(args.exchange, "BTC", "15m", df.index[0])
        ll = rs.test_lead_lag(ton15, btc15, cfg)
        rep.p("**Lead-lag BTC → TON:** кросс-корреляции r_TON(t) vs r_BTC(t−k):")
        rep.table(ll["xcorr"])
        rep.p(f"Granger (IS) p-values по лагам: {json.dumps(ll['granger_p_IS'], default=float)}")
        rep.p("Торговое правило (сильный бар BTC > 2σ, TON отстал → вход на 1 бар), издержки 2×(taker+slip):")
        rep.table(ll["rule"])
    except Exception as e:  # noqa: BLE001
        rep.p(f"Lead-lag не выполнен: {e}")

    if not args.skip_ml:
        print("[ml] ...")
        ml = rs.ml_direction(f, df, cfg)
        rep.p(f"**ML (HistGradientBoosting vs логит vs базовая частота), walk-forward c purge/embargo, "
              f"OOS n={ml['n_oos']}, доля «вверх» = {ml['majority_rate']:.3f}:**")
        rep.table(ml["table"])
        if len(ml["importance"]):
            rep.img(plot_bars(ml["importance"].head(15), rep.path("ml_importance.png"),
                              "Permutation importance (OOS, ΔAUC)", "ΔAUC"), "imp")

    if not args.synthetic:
        ev = pd.read_csv(os.path.join(os.path.dirname(__file__), "events.csv"))
        es = rs.event_study(daily, btc_d, ev)
        rep.p("**Event study (события с источниками в events.csv):**")
        rep.table(es["events"].set_index("date") if len(es["events"]) else es["events"])
        rep.table(es["by_type"])
        if len(es["paths"].columns):
            rep.img(plot_car(es["paths"], rep.path("event_car.png")), "car")

    rep.h("4.1 Специфика инструмента: часы ликвидности и скачки", 3)
    hp = rs.hour_of_day_profile(df)
    rep.img(plot_hours(hp, rep.path("hours.png")), "hours")
    thin = hp.sort_values("amihud_rel", ascending=False).head(4).index.tolist()
    rep.p(f"Самые «тонкие» часы UTC (макс. Amihud): {thin} — в них проскальзывание стоит закладывать выше "
          "(CostModel.thin_hours).")
    rep.table(hp[["vol_share", "abs_ret_bps", "range_bps", "amihud_rel"]])
    jp = rs.jump_analysis(df)
    rep.p(f"Скачки |r| > 5σ: {jp['n_jumps']} ({jp['per_1000_bars']:.2f} на 1000 баров при "
          f"{jp['normal_expected_per_1000']:.4f} для нормального распределения); доля продолжения "
          f"в течение 24 баров: {jp['continuation_share']:.2f}.")

    # ----------------------------------------------------- текущий сигнал
    rep.h("5. Текущий сигнал")
    cs = current_signal(best_name, wfs[best_name], f, df, cfg)
    ok_best = verdicts[best_name][0]
    t = wfs[best_name].oos.trades
    lo, hi = wilson(int((t.net > 0).sum()) if len(t) else 0, len(t))
    rep.p(f"Стратегия с лучшим OOS Sharpe: **{best_name}** "
          f"({'прошла' if ok_best else 'НЕ прошла'} критерии edge). Историческая OOS доля прибыльных сделок: "
          f"{(t.net > 0).mean() if len(t) else float('nan'):.2f}, 90% CI Вильсона [{lo:.2f}; {hi:.2f}] — "
          "это единственная честная «вероятность», которую даёт система.")
    if not ok_best:
        rep.p("> Стратегия не прошла валидацию → сигнал ниже **не является торгуемым**; показан для прозрачности.")
    rep.table({k: (json.dumps(v, default=str) if isinstance(v, dict) else v) for k, v in cs.items()}, "{:.4f}")

    p = rep.save()
    with open(os.path.join(out_dir, "summary.json"), "w") as fh:
        json.dump({"h1": h1, "metrics": summary, "verdicts": {k: v[0] for k, v in verdicts.items()},
                   "validation": {k: {kk: vv for kk, vv in v.items()} for k, v in validations.items()},
                   "current_signal": cs, "n_trials": n_trials}, fh, default=str, indent=1)
    print(f"\nГотово за {time.time() - t0:.0f} c. Отчёт: {p}")
    for name, (ok, checks) in verdicts.items():
        print(f"  {name:14s}: {'EDGE' if ok else 'edge не найден'} | " + " ".join(checks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
