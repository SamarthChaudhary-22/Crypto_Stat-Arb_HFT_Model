import pandas as pd
import os
import statsmodels.api as sm
from statsmodels.tsa.stattools import adfuller
import numpy as np
import time

# --- CONFIGURATION ---
DATA_DIR = "data/raw"
MIN_CORRELATION = 0.85
MAX_HALF_LIFE = 240  # Max 4 hours to revert
P_VALUE_THRESHOLD = 0.05
MIN_OVERLAP_HOURS = 1000  # Minimum ~40 days of overlapping data


def load_and_align_data():
    print("--- Loading and Aligning Data ---")
    files = [f for f in os.listdir(DATA_DIR) if f.endswith('.parquet')]

    if not files:
        print("No data found! Ensure data_loader.py ran successfully.")
        return pd.DataFrame()

    df_combined = pd.DataFrame()

    for file in files:
        symbol = file.replace('.parquet', '')
        path = os.path.join(DATA_DIR, file)

        try:
            df_temp = pd.read_parquet(path)
            if 'open_time' in df_temp.columns:
                df_temp = df_temp.set_index('open_time')

            df_temp = df_temp[['close']].rename(columns={'close': symbol})

            if df_combined.empty:
                df_combined = df_temp
            else:
                df_combined = df_combined.join(df_temp, how='outer')
        except Exception as e:
            print(f"Skipping {file}: {e}")

    df_combined = df_combined.ffill()
    print(f"Loaded {len(df_combined.columns)} assets.")
    return df_combined


def calculate_half_life(spread):
    spread_lag = spread.shift(1)
    spread_ret = spread - spread_lag
    spread_ret = spread_ret.dropna()
    spread_lag = spread_lag.dropna()

    if len(spread_lag) < 100: return np.inf

    try:
        model = sm.OLS(spread_ret, sm.add_constant(spread_lag))
        res = model.fit()
        lam = res.params.iloc[1]

        if lam >= 0: return np.inf
        half_life = -np.log(2) / lam
        return half_life
    except:
        return np.inf


def find_pairs(df):
    # OPTIMIZATION: Resample to 1 Hour candles to speed up math by 60x
    print("--- Resampling to 1H for fast detection ---")
    df_hourly = df.resample('1h').last().dropna(how='all')

    symbols = df_hourly.columns
    pairs = []

    print(f"--- Scanning {len(symbols)} Assets (Speed Mode) ---")
    corr_matrix = df_hourly.corr()

    start_time = time.time()

    for i, sym_a in enumerate(symbols):
        # Progress Bar logic
        if i % 5 == 0:
            print(f"Processing {sym_a} ({i}/{len(symbols)})...")

        for j, sym_b in enumerate(symbols):
            if i >= j: continue

            if corr_matrix.loc[sym_a, sym_b] < MIN_CORRELATION:
                continue

            # Use Hourly data for the test
            df_pair = df_hourly[[sym_a, sym_b]].dropna()

            if len(df_pair) < MIN_OVERLAP_HOURS:
                continue

            S1 = df_pair[sym_a]
            S2 = df_pair[sym_b]

            try:
                x = sm.add_constant(S2)
                model = sm.OLS(S1, x).fit()
                hedge_ratio = model.params.iloc[1]

                spread = S1 - (hedge_ratio * S2)

                adf_result = adfuller(spread)
                p_value = adf_result[1]

                if p_value < P_VALUE_THRESHOLD:
                    hl = calculate_half_life(spread)

                    if hl < MAX_HALF_LIFE and hl > 0:
                        # Convert Hourly Half-Life back to Minutes for context
                        hl_minutes = hl * 60
                        print(f"FOUND: {sym_a} / {sym_b} | P: {p_value:.5f} | HL: {hl_minutes:.0f} min")
                        pairs.append({
                            'leg1': sym_a,
                            'leg2': sym_b,
                            'hedge_ratio': hedge_ratio,
                            'p_value': p_value,
                            'half_life': hl_minutes,
                            'correlation': corr_matrix.loc[sym_a, sym_b]
                        })
            except:
                continue

    print(f"Scan finished in {time.time() - start_time:.2f} seconds")
    return pd.DataFrame(pairs)


if __name__ == "__main__":
    df_prices = load_and_align_data()

    if not df_prices.empty:
        results = find_pairs(df_prices)

        if not results.empty:
            results = results.sort_values(by='p_value')
            results.to_csv("cointegrated_pairs.csv", index=False)
            print("\n>>> SUCCESS: Pairs saved to 'cointegrated_pairs.csv' <<<")
            print("--- Top 5 Pairs ---")
            print(results.head())
        else:
            print("No pairs found.")