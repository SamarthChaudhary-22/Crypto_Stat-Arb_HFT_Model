import pandas as pd
import numpy as np
import requests
import json
import time

# --- CONFIGURATION ---
# List your pairs here exactly as you want them traded
PAIRS_TO_FIX = [
    {"leg1": "ENAUSDT", "leg2": "LINKUSDT", "hedge_ratio": 0.0416},
    {"leg1": "DOTUSDT", "leg2": "BTCUSDT", "hedge_ratio": 12.5},
    # Add more pairs here if you have them, or load from existing file
]

# If you want to load from your BROKEN file instead of typing manually:
LOAD_FROM_FILE = True
FILE_PATH = "strategies.json"


def get_historical_prices(symbol, limit=200):
    """Fetch last 200 candles (1m) to calculate stats."""
    url = f"https://fapi.binance.com/fapi/v1/klines?symbol={symbol}&interval=1m&limit={limit}"
    try:
        resp = requests.get(url).json()
        # Close prices are index 4
        prices = [float(x[4]) for x in resp]
        return np.array(prices)
    except Exception as e:
        print(f"Error fetching {symbol}: {e}")
        return None


def main():
    pairs = PAIRS_TO_FIX

    if LOAD_FROM_FILE:
        try:
            with open(FILE_PATH, "r") as f:
                pairs = json.load(f)
            print(f"Loaded {len(pairs)} pairs from {FILE_PATH}")
        except:
            print("Could not load file. Using hardcoded list.")

    corrected_strategies = []

    print("--- 🛠️ RECALCULATING MATH PARAMETERS ---")

    for p in pairs:
        leg1 = p.get("leg1") or p.get("asset1")  # Handle both naming conventions
        leg2 = p.get("leg2") or p.get("asset2")
        ratio = float(p["hedge_ratio"])

        if not leg1 or not leg2:
            continue

        print(f"Processing {leg1}/{leg2}...", end=" ")

        # 1. Get Data
        p1_data = get_historical_prices(leg1)
        p2_data = get_historical_prices(leg2)

        if p1_data is None or p2_data is None or len(p1_data) != len(p2_data):
            print("❌ Data Error (Mismatch or Empty)")
            continue

        # 2. Calculate Spread
        # Spread = log(P1) - ratio * log(P2)
        spreads = np.log(p1_data) - (ratio * np.log(p2_data))

        # 3. Calculate Stats
        mean = float(np.mean(spreads))
        std_dev = float(np.std(spreads))

        print(f"✅ Mean: {mean:.5f} | Std: {std_dev:.5f}")

        # 4. Save
        new_entry = {
            "leg1": leg1,
            "leg2": leg2,
            "hedge_ratio": ratio,
            "mean": mean,  # <--- THIS WAS MISSING
            "std_dev": std_dev  # <--- THIS WAS MISSING
        }
        corrected_strategies.append(new_entry)

    # Save to file
    with open("strategies.json", "w") as f:
        json.dump(corrected_strategies, f, indent=4)

    print(f"\nSUCCESS! Overwrote strategies.json with {len(corrected_strategies)} valid pairs.")


if __name__ == "__main__":
    main()