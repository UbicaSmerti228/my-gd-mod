"""
report.py — итоговый отчёт: markdown + PNG-графики (matplotlib, без интерактива).
Палитра — фиксированный порядок категориальных цветов (не циклится), тонкие линии,
приглушённые сетка/оси, одна ось Y на график.
"""
from __future__ import annotations

import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
MUTED, TEXT, GRID, SURFACE = "#52514e", "#0b0b0b", "#e4e3df", "#fcfcfb"
REGIME_FILL = {"trend": "#dbe9fa", "range": "#f1f0ec", "high_vol": "#fbe0d4"}

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "axes.edgecolor": GRID,
    "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED, "text.color": TEXT,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 1.6, "font.size": 9, "legend.frameon": False,
})


def _save(fig, path):
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)
    return os.path.basename(path)


def plot_equity(curves: dict[str, pd.Series], path: str, title: str) -> str:
    fig, ax = plt.subplots(figsize=(10, 4.2))
    for k, (name, s) in enumerate(curves.items()):
        s = s[s > 0]
        ax.plot(s.index, s / s.iloc[0], label=name, color=SERIES[k % len(SERIES)] if k < 8 else MUTED,
                lw=2.2 if name.startswith("Buy") else 1.4)
    ax.set_yscale("log"); ax.set_ylabel("капитал, x (лог. шкала)")
    ax.set_title(title, loc="left")
    ax.legend(ncol=6, loc="upper center", bbox_to_anchor=(0.5, -0.12))
    return _save(fig, path)


def plot_drawdown(curves: dict[str, pd.Series], path: str) -> str:
    fig, ax = plt.subplots(figsize=(10, 3))
    for k, (name, s) in enumerate(curves.items()):
        ax.plot(s.index, (s / s.cummax() - 1) * 100, label=name, color=SERIES[k % len(SERIES)], lw=1.2)
    ax.set_ylabel("просадка, %"); ax.set_title("Просадки (OOS)", loc="left")
    ax.legend(ncol=6, loc="upper center", bbox_to_anchor=(0.5, -0.15))
    return _save(fig, path)


def plot_regimes(close: pd.Series, regime: pd.Series, path: str) -> str:
    fig, ax = plt.subplots(figsize=(10, 3.6))
    r = regime.reindex(close.index)
    blocks = (r != r.shift()).cumsum()
    for _, g in r.groupby(blocks):
        lab = g.iloc[0]
        if isinstance(lab, str):
            ax.axvspan(g.index[0], g.index[-1], color=REGIME_FILL.get(lab, "#eee"), lw=0)
    ax.plot(close.index, close, color=TEXT, lw=0.9)
    ax.set_yscale("log"); ax.set_title("Цена и режим (голубой — trend, серый — range, оранжевый — high_vol)", loc="left")
    return _save(fig, path)


def plot_hist(values, ref: float | None, path: str, title: str, xlabel: str) -> str:
    fig, ax = plt.subplots(figsize=(6, 3.4))
    ax.hist(np.asarray(values)[np.isfinite(values)], bins=40, color=SERIES[0], alpha=0.85, edgecolor=SURFACE)
    if ref is not None and np.isfinite(ref):
        ax.axvline(ref, color=SERIES[1], lw=2); ax.annotate("стратегия", (ref, ax.get_ylim()[1] * 0.9), color=TEXT)
    ax.set_title(title, loc="left"); ax.set_xlabel(xlabel)
    return _save(fig, path)


def plot_heatmap(tab: pd.DataFrame, path: str, title: str) -> str:
    fig, ax = plt.subplots(figsize=(5.5, 3.6))
    v = tab.to_numpy(float)
    lim = np.nanmax(np.abs(v)) if np.isfinite(v).any() else 1
    im = ax.imshow(v, cmap="RdBu", vmin=-lim, vmax=lim, aspect="auto")
    ax.set_xticks(range(tab.shape[1]), tab.columns); ax.set_yticks(range(tab.shape[0]), tab.index)
    ax.set_xlabel(tab.columns.name); ax.set_ylabel(tab.index.name); ax.grid(False)
    for i in range(v.shape[0]):
        for j in range(v.shape[1]):
            if np.isfinite(v[i, j]):
                ax.text(j, i, f"{v[i, j]:.2f}", ha="center", va="center", color=TEXT, fontsize=8)
    fig.colorbar(im, ax=ax, shrink=0.8, label="медианный Sharpe (train)")
    ax.set_title(title, loc="left")
    return _save(fig, path)


def plot_bars(s: pd.Series, path: str, title: str, xlabel: str) -> str:
    fig, ax = plt.subplots(figsize=(7, max(2.5, 0.25 * len(s))))
    s = s.iloc[::-1]
    ax.barh(range(len(s)), s.values, color=SERIES[0], height=0.7)
    ax.set_yticks(range(len(s)), s.index); ax.set_xlabel(xlabel); ax.set_title(title, loc="left")
    return _save(fig, path)


def plot_hours(prof: pd.DataFrame, path: str) -> str:
    fig, ax = plt.subplots(figsize=(8, 3))
    ax.bar(prof.index, prof["vol_share"] * 100, color=SERIES[0], width=0.8)
    ax.set_xlabel("час UTC"); ax.set_ylabel("доля объёма, %")
    ax.set_title("Ликвидность по часам (доля дневного объёма)", loc="left")
    return _save(fig, path)


def plot_car(paths: pd.DataFrame, path: str) -> str:
    fig, ax = plt.subplots(figsize=(8, 3.6))
    for k, c in enumerate(paths.columns[:8]):
        ax.plot(paths.index, paths[c] * 100, color=SERIES[k], lw=1.3, label=str(c)[:40])
    ax.axvline(0, color=MUTED, lw=0.8); ax.axhline(0, color=MUTED, lw=0.8)
    ax.set_xlabel("дни от события"); ax.set_ylabel("CAR, %"); ax.legend(fontsize=7)
    ax.set_title("Event study: кумулятивная аномальная доходность (vs BTC-модель)", loc="left")
    return _save(fig, path)


# -------------------------------------------------------------- markdown
def md_table(df: pd.DataFrame | dict, floatfmt: str = "{:.3f}") -> str:
    if isinstance(df, dict):
        df = pd.DataFrame([df]).T.rename(columns={0: "value"})
    if df is None or len(df) == 0:
        return "_нет данных_\n"
    df = df.copy()

    def fmt(v):
        if isinstance(v, (float, np.floating)):
            return "—" if not np.isfinite(v) else floatfmt.format(v)
        return str(v)
    cols = [str(c) for c in df.columns]
    head = "| " + " | ".join([str(df.index.name or "")] + cols) + " |"
    sep = "|" + "---|" * (len(cols) + 1)
    rows = ["| " + " | ".join([str(i)] + [fmt(v) for v in r]) + " |" for i, r in zip(df.index, df.to_numpy())]
    return "\n".join([head, sep] + rows) + "\n"


class Report:
    def __init__(self, out_dir: str, title: str):
        self.dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.parts = [f"# {title}\n"]

    def h(self, text: str, level: int = 2):
        self.parts.append(f"\n{'#' * level} {text}\n")

    def p(self, text: str):
        self.parts.append(text.rstrip() + "\n")

    def table(self, df, floatfmt="{:.3f}"):
        self.parts.append(md_table(df, floatfmt))

    def img(self, fname: str, alt: str = ""):
        self.parts.append(f"\n![{alt}]({fname})\n")

    def path(self, name: str) -> str:
        return os.path.join(self.dir, name)

    def save(self, name: str = "report.md") -> str:
        p = os.path.join(self.dir, name)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write("\n".join(self.parts))
        return p
