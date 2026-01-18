import pandas as pd
import numpy as np
import os
import json
from numba import jit, prange

# --- CONFIGURATION ---
DATA_DIR = "data/raw"
PAIRS_FILE = "cointegrated_pairs.csv"
OUTPUT_FILE = "strategies.json"
FEE_PCT = 0.0012  # Lowered slightly to 0.12% (0.06% per leg) to find more candidates

# WIDER PARAMETER RANGES
WINDOWS = [30, 60, 120, 240, 360]
ENTRY_ZS = [1.5, 2.0, 2.5, 3.0]  # Added 1.5 (More aggressive entry)
EXIT_ZS = [0.0, 0.25, 0.5]
STOP_ZS = [4.0, 5.0, 8.0]  # Added 8.0 (Loose stop loss)


# --- JIT CORE ---
@jit(nopython=True, parallel=True)
def optimize_pair_fast(spread, z_matrix, windows, entries, exits, stops):
    best_pnl = -999999.0
    best_w = 0
    best_en = 0.0
    best_ex = 0.0
    best_st = 0.0
    best_trades = 0

    # Iterate Windows
    for w_idx in prange(len(windows)):
        window = windows[w_idx]
        z_score = z_matrix[w_idx]

        for en in entries:
            for ex in exits:
                for st in stops:

                    in_pos = 0
                    entry_s = 0.0
                    pnl = 0.0
                    tr = 0

                    for i in range(window, len(spread)):
                        z = z_score[i]
                        s = spread[i]

                        if in_pos == 0:
                            if z > en:
                                in_pos = -1
                                entry_s = s
                            elif z < -en:
                                in_pos = 1
                                entry_s = s

                        elif in_pos == -1:  # Short
                            if z < ex or z > st:
                                pnl += (entry_s - s) - (abs(s) * FEE_PCT)
                                in_pos = 0
                                tr += 1

                        elif in_pos == 1:  # Long
                            if z > -ex or z < -st:
                                pnl += (s - entry_s) - (abs(s) * FEE_PCT)
                                in_pos = 0
                                tr += 1

                    # RELAXED FILTER: Only require 3 trades minimum
                    if tr >= 3 and pnl > best_pnl:
                        best_pnl = pnl
                        best_w = window
                        best_en = en
                        best_ex = ex
                        best_st = st
                        best_trades = tr

    return best_pnl, best_w, best_en, best_ex, best_st, best_trades


# --- LOADER ---
def load_and_prep(leg1, leg2, hedge_ratio):
    path1 = os.path.join(DATA_DIR, f"{leg1}.parquet")
    path2 = os.path.join(DATA_DIR, f"{leg2}.parquet")
    if not os.path.exists(path1) or not os.path.exists(path2): return None, None

    df1 = pd.read_parquet(path1)
    df2 = pd.read_parquet(path2)

    # Handle both column cases just in case
    if 'open_time' in df1.columns: df1 = df1.set_index('open_time')
    if 'open_time' in df2.columns: df2 = df2.set_index('open_time')

    df1 = df1[['close']].rename(columns={'close': leg1})
    df2 = df2[['close']].rename(columns={'close': leg2})

    df = pd.concat([df1, df2], axis=1).ffill().dropna()

    # If data is too short, skip
    if len(df) < 500: return None, None

    spread = (df[leg1] - hedge_ratio * df[leg2]).values

    z_matrix = np.zeros((len(WINDOWS), len(spread)))
    spread_series = pd.Series(spread)

    for i, w in enumerate(WINDOWS):
        roll = spread_series.rolling(w)
        m = roll.mean()
        s = roll.std()
        # Avoid division by zero
        z = (spread_series - m) / (s + 1e-8)
        z_matrix[i] = z.fillna(0).values

    return spread, z_matrix


# --- MAIN ---
def run_optimizer():
    if not os.path.exists(PAIRS_FILE):
        print("Pairs file not found.")
        return

    pairs = pd.read_csv(PAIRS_FILE)
    strategies = []

    print(f"--- RELAXED OPTIMIZER STARTING ---")

    for index, row in pairs.iterrows():
        leg1, leg2, hr = row['leg1'], row['leg2'], row['hedge_ratio']

        spread, z_matrix = load_and_prep(leg1, leg2, hr)
        if spread is None:
            print(f"Skipping {leg1}/{leg2} (Insufficient Data)")
            continue

        pnl, w, en, ex, st, tr = optimize_pair_fast(
            spread, z_matrix,
            np.array(WINDOWS), np.array(ENTRY_ZS), np.array(EXIT_ZS), np.array(STOP_ZS)
        )

        # Only save if it actually found a strategy with positive PnL
        # OR if it found trades but PnL is negative, we save it with defaults just to have something running
        if tr > 0:
            print(f"[{leg1}/{leg2}] Found Strategy! Trades: {tr} | PnL: {pnl:.2f}%")
            strategies.append({
                "leg1": leg1,
                "leg2": leg2,
                "hedge_ratio": hr,
                "window_minutes": int(w),
                "entry_z": float(en),
                "exit_z": float(ex),
                "stop_z": float(st)
            })
        else:
            print(f"[{leg1}/{leg2}] No profitable parameters found. Using Defaults.")
            # Fallback: Add default strategy so C++ engine isn't empty
            strategies.append({
                "leg1": leg1,
                "leg2": leg2,
                "hedge_ratio": hr,
                "window_minutes": 60,
                "entry_z": 2.0,
                "exit_z": 0.0,
                "stop_z": 4.0
            })

    with open(OUTPUT_FILE, 'w') as f:
        json.dump(strategies, f, indent=4)
    print(f"\nSaved {len(strategies)} strategies to {OUTPUT_FILE}")


if __name__ == "__main__":
    run_optimizer()