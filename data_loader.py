import requests
import pandas as pd
import time
from datetime import datetime, timedelta
import os

# --- CONFIGURATION ---
BINANCE_FUTURES_URL = "https://fapi.binance.com"
TIMEFRAME = "1m"  # 1-minute candles for granular testing
LOOKBACK_DAYS = 365  # 1 Year of data
TOP_N_COINS = 50  # Filter: Only look at the top 50 most liquid coins
DATA_DIR = "data/raw"  # Folder to save data

# Ensure data directory exists
os.makedirs(DATA_DIR, exist_ok=True)


# --- UTILS ---
def get_top_liquid_assets(limit=50):
    """
    Step 1: Universe Selection
    Fetches all USDT-M pairs and selects the top 'limit' by Dollar Volume.
    """
    print(f"--- Fetching Top {limit} Liquid Assets ---")
    try:
        url = f"{BINANCE_FUTURES_URL}/fapi/v1/ticker/24hr"
        response = requests.get(url)
        data = response.json()

        # Convert to DataFrame
        df = pd.DataFrame(data)

        # Filter: Only trade USDT pairs (exclude USDC, BUSD, etc for simplicity)
        df = df[df['symbol'].str.endswith('USDT')]

        # Convert quoteVolume to float and sort
        df['quoteVolume'] = df['quoteVolume'].astype(float)
        df = df.sort_values(by='quoteVolume', ascending=False)

        # Select top N
        top_assets = df['symbol'].head(limit).tolist()
        print(f"Top Assets identified: {top_assets[:5]} ...")
        return top_assets
    except Exception as e:
        print(f"Error fetching top assets: {e}")
        return []


def fetch_klines(symbol, start_ts, end_ts):
    """
    Fetches a single batch of 1500 candles.
    """
    url = f"{BINANCE_FUTURES_URL}/fapi/v1/klines"
    params = {
        'symbol': symbol,
        'interval': TIMEFRAME,
        # 'startTime': start_ts,
        'endTime': end_ts,
        'limit': 1500  # Max limit for Binance Futures
    }

    try:
        response = requests.get(url, params=params)
        if response.status_code == 200:
            return response.json()
        elif response.status_code == 429:
            print("!!! RATE LIMIT HIT. Sleeping for 60s !!!")
            time.sleep(60)
            return None
        else:
            print(f"Error {response.status_code}: {response.text}")
            return None
    except Exception as e:
        print(f"Connection error: {e}")
        return None


def download_history(symbol):
    """
    Step 2: The Pagination Loop
    Downloads data chunk by chunk from Today backwards to Start Date.
    """
    print(f"Downloading {symbol}...")

    # Time Setup
    end_date = datetime.now()
    start_date = end_date - timedelta(days=LOOKBACK_DAYS)

    # Timestamps in milliseconds
    end_ts = int(end_date.timestamp() * 1000)
    start_ts = int(start_date.timestamp() * 1000)

    all_kline_data = []
    current_end_ts = end_ts

    while True:
        # Fetch batch
        klines = fetch_klines(symbol, start_ts, current_end_ts)

        if not klines:
            break

        # Append data
        all_kline_data.extend(klines)

        # Update timestamp: Oldest candle in this batch becomes the endTime for next batch
        # klines[0][0] is the Open Time of the first (oldest) candle in the batch
        oldest_ts_in_batch = klines[0][0]
        current_end_ts = oldest_ts_in_batch - 1

        # Break if we reached the start time or no new data
        if current_end_ts <= start_ts:
            break

        print(f"   Fetched batch ending {datetime.fromtimestamp(oldest_ts_in_batch / 1000)}")

        # Step 3: Rate Limiting (Crucial for resume projects to show care)
        time.sleep(0.1)

    if not all_kline_data:
        return False

    # Process Data
    cols = ['open_time', 'open', 'high', 'low', 'close', 'volume', 'close_time',
            'quote_volume', 'trades', 'taker_buy_vol', 'taker_buy_quote_vol', 'ignore']

    df = pd.DataFrame(all_kline_data, columns=cols)

    # Clean types
    df['open_time'] = pd.to_datetime(df['open_time'], unit='ms')
    df['close'] = df['close'].astype(float)
    df = df[['open_time', 'close']]  # We only need Close price for Stat Arb

    # Sort chronological (Binance sends usually oldest first in batch, but our logic went backwards)
    df = df.sort_values('open_time').reset_index(drop=True)
    df = df.drop_duplicates(subset=['open_time'])

    # Step 4: Save to Parquet
    filename = f"{DATA_DIR}/{symbol}.parquet"
    df.to_parquet(filename)
    print(f"Saved {len(df)} rows to {filename}")
    return True


# --- MAIN EXECUTION ---
if __name__ == "__main__":
    # 1. Get Universe
    symbols = get_top_liquid_assets(TOP_N_COINS)

    # 2. Loop and Download
    for symbol in symbols:
        success = download_history(symbol)
        if not success:
            print(f"Failed to download {symbol}")

    print("\n>>> Data Download Complete. Ready for Analysis. <<<")