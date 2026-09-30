# ton_quant — исследовательский фреймворк для перпетуалов TON/GRAM (USDT-M)

> Это исследовательский инструмент, а не финансовая рекомендация. Фьючерсы с плечом
> могут привести к потере всего депозита.

## Статус (2026-09-30)

* Код фреймворка готов, 9 юнит-тестов проходят (в том числе тесты на отсутствие look-ahead
  в признаках, сигналах и HMM-режимах).
* **Прогона на реальных данных пока не было.** Контейнер, в котором писался код, не имеет
  сетевого доступа к `data.binance.vision`, `fapi.binance.com`, `api.bybit.com`, `www.okx.com`
  (egress proxy отвечает 403). Поэтому **в репозитории нет ни одной цифры бэктеста по TON** —
  выдумывать их нельзя. На машине с интернетом всё запускается одной командой (см. ниже).
* Фреймворк проверен на синтетике в двух режимах (`reports/synthetic_*`):
  * `null` — случайное блуждание без предсказуемости: ни одна стратегия не проходит критерии
    edge (контроль ложноположительных сработал);
  * `planted` — вшит слабый контрарный эффект funding: исследовательский тест находит его
    OOS (|t| > 5), а DSR с поправкой на 44 испытания остаётся строгим.
    (Отчёт `synthetic_planted` сгенерирован до последней правки графиков и границ бенчмарка;
    для актуальной версии: `python run.py --synthetic planted --fast`.)

## Важная особенность инструмента: ребрендинг TON → GRAM

| Дата (UTC) | Событие | Источник |
|---|---|---|
| 2026-06-01…08 | Голосование о переименовании Toncoin → Gram, 81.22% «за» | [cryptotimes.io](https://www.cryptotimes.io/2026/07/02/gram-jumps-5-as-binance-completes-ton-to-gram-rebrand/) |
| 2026-06-15 | Ребрендинг вступил в силу; Bybit снял все TON perpetual (09:00) | [Bybit](https://announcements.bybit.com/en/article/bybit-to-support-toncoin-ton-rebrand-and-ticker-change-to-gram-gram--blt617f4bf48950ab77/) |
| 2026-06-23 | Binance закрыл и рассчитал TONUSDT perp (09:00) | [crypto.news](https://crypto.news/binance-drops-ton-ticker-as-gram-trading-starts-july-2/) |
| 2026-07-02 | Binance открыл GRAMUSDT perp (до 50x), 08:00 | [bitcoinworld](https://bitcoinworld.co.in/binance-gram-perpetual-futures-50x-leverage/) |

История контракта — это склейка двух символов с разрывом ~9 дней. `data.load_perp`
склеивает их по `config.SYMBOL_HISTORY`, помечает `gap_before`, а бэктест **не держит позицию
через разрыв** (закрывает по последнему close перед делистингом). Даты перехода на Bybit
(старт GRAM perp) и OKX поиском не подтверждены — помечены `TODO(verify)` в `config.py`.
Исторические данные снятого TONUSDT доступны через архив `data.binance.vision`, поэтому
Binance — основной источник; ccxt REST обычно не отдаёт снятые рынки.

## Запуск

```bash
cd ton_quant
pip install -r requirements.txt
python -m pytest -q tests                 # тесты корректности
python run.py                             # Binance, 1h, с 2022-01-01 -> reports/binance/report.md
python run.py --exchange bybit            # через ccxt (история TONUSDT может быть недоступна)
python run.py --synthetic null --fast     # самопроверка без сети
```

Что нужно для реального прогона: доступ к `https://data.binance.vision` (≈1–2 тыс. zip-файлов,
кэшируются в `data_cache/`), для 15m lead-lag — те же архивы 15m. Для Bybit/OKX — их REST API.
Ключи API не нужны (только публичные данные). Если данных нет, `run.py` останавливается с
кодом 2 и пишет, чего не хватает; синтетикой ничего не подменяется.

## Структура

| Файл | Что делает |
|---|---|
| `config.py` | seed, издержки (taker/maker/slippage/тонкие часы), риск (плечо, риск на сделку, MMR), окна WF, история символов |
| `data.py` | загрузка OHLCV/taker-volume, funding, premium index (basis), 5-мин metrics (OI, L/S) из архива Binance; ccxt-путь для Bybit/OKX; parquet-кэш; контроль пропусков/дублей/OHLC/выбросов (MAD) |
| `features.py` | EMA/SMA-кроссы, ADX, наклон регрессии + t, Supertrend; RSI, MACD, StochRSI, ROC; ATR, Bollinger, Keltner, RV, squeeze; OBV, VWAP (скользящий и сессионный), CVD, volume profile/POC; funding z, OI-квадранты, basis z, L/S z; бета/корреляция/lead-lag к BTC/ETH, RS TON/BTC; 4h-тренд через каузальное выравнивание |
| `regime.py` | GaussianHMM (3 состояния), обучение только на train, **forward-фильтр** вместо Viterbi (у `hmmlearn.predict` есть look-ahead); rule-based baseline |
| `signals.py` | 5 стратегий (trend, meanrev, squeeze, funding-contrarian, regime_switch) + confluence-скор из 8 независимых факторов |
| `backtest.py` | событийный бэктест: вход на open t+1, taker/maker, проскальзывание (x2 в тонкие часы), funding по факту, изолированная маржа и ликвидация, гэп через стоп, стоп раньше тейка при неоднозначности; метрики и разбивки по режимам/годам |
| `validation.py` | walk-forward с робастным выбором (медиана Sharpe по соседям в сетке), purged K-fold с эмбарго, Monte Carlo (перестановка/bootstrap), stationary bootstrap CI Sharpe, Deflated Sharpe, White's Reality Check, случайные входы |
| `research.py` | этап 4: funding-контрарианство, OI+цена+объём, прокси «магнитов» ликвидаций, lead-lag BTC (15m, Грейнджер, торговое правило с издержками), Калман/вейвлет (каузальные), ML (HistGB vs логит vs базовая частота, permutation importance), event study, часы ликвидности, скачки, H1-тест, уровни |
| `report.py` | markdown-отчёт + PNG |
| `run.py` | оркестратор всех этапов, вердикты по заранее заявленным критериям |
| `synthetic.py` | синтетика **только** для самопроверки фреймворка |
| `events.csv` | события для event study (каждое с источником) |

## Заранее заявленные критерии edge (все обязательны)

1. ≥ 30 OOS-сделок; 2. OOS Sharpe > 0.5 после всех издержек; 3. Deflated Sharpe > 0.95
(N = все испытанные конфигурации, сейчас 44); 4. White's Reality Check p < 0.05;
5. лучше случайных входов (p < 0.05) при тех же издержках и длительностях; 6. Monte Carlo
P(убыток) < 20%. Не выполнено хоть одно — «edge не найден».

## Известные ограничения

* Карта ликвидаций — **приближение** по приросту OI и типичным плечам (исторических
  данных о ликвидациях в открытом доступе нет).
* Funding по Bybit/OKX через ccxt ограничен глубиной их API; metrics (OI/L-S) полностью
  есть только в архиве Binance.
* Цена ликвидации — упрощённая формула для одного tier MMR; реальные биржи используют
  ступенчатые tiers.
* HMM-режимы на коротком train (180 дней) могут быть неустойчивы — при сбое используется
  rule-based режим, это пишется в лог.
* Event study по 7 событиям имеет низкую мощность; результаты — описательные.
