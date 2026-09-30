"""
regime.py — определение рыночного режима: trend / range / high_vol.

Два метода:
  * HMMRegime — GaussianHMM (hmmlearn), обучается ТОЛЬКО на train-окне,
    а на test применяется ФИЛЬТРАЦИЯ (forward-алгоритм), а не Viterbi/сглаживание:
    predict()/predict_proba() hmmlearn используют будущие наблюдения
    (forward-backward) — это look-ahead, поэтому здесь свой forward-фильтр.
  * rule_regime — прозрачный baseline на ADX и каузальном перцентиле волатильности.
"""
from __future__ import annotations

import numpy as np
import pandas as pd
from scipy.special import logsumexp
from scipy.stats import multivariate_normal

from config import SEED

LABELS = ("trend", "range", "high_vol")


def regime_inputs(f: pd.DataFrame) -> pd.DataFrame:
    """Наблюдения для HMM: |доходность за 24 бара|, реализ. волатильность, сила тренда."""
    x = pd.DataFrame({
        "abs_ret24": np.log1p(f["ret1"].rolling(24).sum().abs()),
        "log_rv": np.log(f["rv_24"] + 1e-6),
        "trend_t": f["slope_t"].abs().clip(upper=20),
        "adx": f["adx"],
    }, index=f.index)
    return x


class HMMRegime:
    def __init__(self, n_states: int = 3, seed: int = SEED):
        self.n_states, self.seed = n_states, seed
        self.model = None
        self.mu = self.sd = None
        self.state_label: dict[int, str] = {}

    def fit(self, x: pd.DataFrame) -> "HMMRegime":
        from hmmlearn.hmm import GaussianHMM
        x = x.dropna()
        self.mu, self.sd = x.mean(), x.std() + 1e-9
        z = ((x - self.mu) / self.sd).values
        best, best_ll = None, -np.inf
        for k in range(3):  # несколько инициализаций, выбираем по log-likelihood на train
            m = GaussianHMM(self.n_states, covariance_type="full", n_iter=200,
                            random_state=self.seed + k, min_covar=1e-3)
            m.fit(z)
            ll = m.score(z)
            if ll > best_ll:
                best, best_ll = m, ll
        self.model = best
        # Маркировка состояний по средним (в исходных единицах).
        means = pd.DataFrame(best.means_ * self.sd.values + self.mu.values, columns=x.columns)
        hv = int(means["log_rv"].idxmax())
        rest = [i for i in range(self.n_states) if i != hv]
        tr = max(rest, key=lambda i: means.loc[i, "trend_t"] + means.loc[i, "adx"] / 10)
        self.state_label = {i: "range" for i in range(self.n_states)}
        self.state_label[hv], self.state_label[tr] = "high_vol", "trend"
        self.state_means = means
        return self

    def filter_proba(self, x: pd.DataFrame) -> pd.DataFrame:
        """Каузальные P(state_t | obs_1..t) — forward-алгоритм."""
        m = self.model
        valid = x.notna().all(axis=1)
        z = ((x[valid] - self.mu) / self.sd).values
        logB = np.column_stack([multivariate_normal(m.means_[s], m.covars_[s], allow_singular=True).logpdf(z)
                                for s in range(self.n_states)])
        logA = np.log(m.transmat_ + 1e-300)
        la = np.log(m.startprob_ + 1e-300) + logB[0]
        out = np.empty_like(logB)
        out[0] = la - logsumexp(la)
        for t in range(1, len(z)):
            la = logsumexp(out[t - 1][:, None] + logA, axis=0) + logB[t]
            out[t] = la - logsumexp(la)
        p = pd.DataFrame(np.exp(out), index=x.index[valid],
                         columns=[self.state_label[s] for s in range(self.n_states)])
        p = p.T.groupby(level=0).sum().T  # если две «range»-метки — складываем
        return p.reindex(x.index)

    def predict(self, x: pd.DataFrame) -> pd.Series:
        p = self.filter_proba(x)
        ok = p.notna().all(axis=1)
        out = pd.Series(np.nan, index=x.index, dtype=object)
        out[ok] = p[ok].idxmax(axis=1)
        return out


def rule_regime(f: pd.DataFrame, adx_thr: float = 25.0, vol_pct_thr: float = 0.85) -> pd.Series:
    """Прозрачный baseline. high_vol имеет приоритет."""
    r = pd.Series("range", index=f.index, dtype=object)
    r[f["adx"] >= adx_thr] = "trend"
    r[f["rv_pctile"] >= vol_pct_thr] = "high_vol"
    return r.where(f[["adx", "rv_pctile"]].notna().all(axis=1))


def fit_predict_regime(f: pd.DataFrame, train_idx: pd.Index, method: str = "hmm") -> pd.Series:
    """Обучить на train, выдать каузальные режимы на всём f (train+test)."""
    if method == "rule":
        return rule_regime(f)
    x = regime_inputs(f)
    try:
        hmm = HMMRegime().fit(x.loc[train_idx])
        return hmm.predict(x)
    except Exception as e:  # noqa: BLE001 — при неудаче HMM откатываемся на правило и сообщаем
        print(f"[regime] HMM не сошёлся ({e}); используется rule_regime")
        return rule_regime(f)
