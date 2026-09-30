"""
Глобальная конфигурация исследовательского фреймворка TON/GRAM perpetuals.

ВАЖНО (проверено поиском 2026-09-30, см. README):
  * 15.06.2026 токен Toncoin переименован в Gram (тикер TON -> GRAM), своп 1:1.
  * Binance Futures: TONUSDT perp закрыт и рассчитан 23.06.2026 09:00 UTC,
    GRAMUSDT perp открыт 02.07.2026 08:00 UTC  (crypto.news, bitcoinworld.co.in).
  * Bybit: все TON perpetual сняты 15.06.2026 09:00 UTC, GRAM spot с 22.06.2026
    (announcements.bybit.com). Дата старта GRAM perp на Bybit НЕ подтверждена.
  * OKX: даты перехода НЕ подтверждены поиском.
Поэтому история контракта = склейка двух символов с разрывом. Даты в
SYMBOL_HISTORY для Bybit/OKX помечены как требующие проверки.
"""
from __future__ import annotations

import os
import random
from dataclasses import dataclass, field

import numpy as np

SEED = 42

ROOT = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(ROOT, "data_cache")
REPORT_DIR = os.path.join(ROOT, "reports")


def set_seed(seed: int = SEED) -> None:
    """Фиксируем все генераторы случайных чисел для воспроизводимости."""
    random.seed(seed)
    np.random.seed(seed)
    os.environ["PYTHONHASHSEED"] = str(seed)


# Длительность таймфреймов в pandas-нотации (pandas>=2.2: 'min', 'h', 'D').
TF_PANDAS = {"5m": "5min", "15m": "15min", "1h": "1h", "4h": "4h", "1d": "1D"}

# (символ ccxt / символ Binance-архива, начало, конец) — None = без ограничения.
SYMBOL_HISTORY = {
    "binance": [
        {"ccxt": "TON/USDT:USDT", "raw": "TONUSDT", "start": None, "end": "2026-06-23T09:00:00Z"},
        {"ccxt": "GRAM/USDT:USDT", "raw": "GRAMUSDT", "start": "2026-07-02T08:00:00Z", "end": None},
    ],
    "bybit": [
        {"ccxt": "TON/USDT:USDT", "raw": "TONUSDT", "start": None, "end": "2026-06-15T09:00:00Z"},
        # TODO(verify): дата запуска GRAMUSDT perp на Bybit не подтверждена.
        {"ccxt": "GRAM/USDT:USDT", "raw": "GRAMUSDT", "start": "2026-06-22T08:00:00Z", "end": None},
    ],
    "okx": [
        # TODO(verify): даты перехода TON-USDT-SWAP -> GRAM-USDT-SWAP на OKX не подтверждены.
        {"ccxt": "TON/USDT:USDT", "raw": "TON-USDT-SWAP", "start": None, "end": None},
        {"ccxt": "GRAM/USDT:USDT", "raw": "GRAM-USDT-SWAP", "start": None, "end": None},
    ],
}

BENCH_SYMBOLS = {
    "binance": {"BTC": "BTCUSDT", "ETH": "ETHUSDT"},
    "bybit": {"BTC": "BTC/USDT:USDT", "ETH": "ETH/USDT:USDT"},
    "okx": {"BTC": "BTC/USDT:USDT", "ETH": "ETH/USDT:USDT"},
}


@dataclass
class CostModel:
    """Издержки. Значения по умолчанию ~ базовый VIP0 tier Binance USDT-M."""
    taker_fee: float = 0.0005      # 5 bps
    maker_fee: float = 0.0002      # 2 bps
    slippage_bps: float = 3.0      # базовое проскальзывание рыночного ордера
    # Множитель проскальзывания в «тонкие» часы (UTC), задаётся после анализа
    # hour-of-day ликвидности (research.hour_of_day_profile).
    thin_hours: tuple = ()
    thin_hours_mult: float = 2.0
    tp_is_maker: bool = True       # тейк-профит — лимитный ордер (maker)


@dataclass
class RiskModel:
    leverage: float = 3.0            # максимальное плечо (ограничивает номинал)
    risk_per_trade: float = 0.01     # 1% капитала на сделку (расстояние до стопа)
    maint_margin_rate: float = 0.01  # ставка поддерживающей маржи (консервативно для альткоина)
    initial_capital: float = 10_000.0
    max_bars_in_trade: int = 24 * 7  # тайм-стоп (в барах рабочего ТФ)


@dataclass
class WFConfig:
    """Walk-forward: окна в барах рабочего ТФ (1h по умолчанию)."""
    train_bars: int = 24 * 180   # ~6 месяцев
    test_bars: int = 24 * 60     # ~2 месяца
    step_bars: int = 24 * 60
    min_trades_train: int = 15


@dataclass
class ResearchConfig:
    exchange: str = "binance"
    base_tf: str = "1h"
    since: str = "2022-01-01"
    oos_fraction: float = 0.3       # доля истории, которую исследовательские тесты (этап 4) видят только как OOS
    mc_sims: int = 2000
    random_entry_sims: int = 300
    bootstrap_block: int = 10       # средняя длина блока (дни) для stationary bootstrap
    costs: CostModel = field(default_factory=CostModel)
    risk: RiskModel = field(default_factory=RiskModel)
    wf: WFConfig = field(default_factory=WFConfig)
