# Crypto Stat-Arb HFT Engine 

A high-frequency trading (HFT) execution engine written in C++20, designed for statistical arbitrage on Binance Futures.

## Features
- **Ultra-Low Latency:** ~10ms execution speed (Colocated in AWS Tokyo).
- **Architecture:** Lock-free circular buffers & C++20 coroutines.
- **Strategy:** Mean Reversion / Statistical Arbitrage.
- **Infrastructure:** AWS EC2 (t3.micro), Ubuntu 24.04 / Amazon Linux.

## Tech Stack
- **Language:** C++20
- **Libraries:** CPR (Network), nlohmann/json (Parsing), OpenSSL (HMAC SHA256)
- **Deployment:** AWS EC2 (ap-northeast-1)

## 🧬 Architecture
This project follows a professional "Research-to-Production" workflow:
1.  **Research Layer (Python):** -   `data_loader.py`: Fetches OHLCV market data.
    -   `analyzer.py`: Vectorized backtesting of mean-reversion signals using Pandas.
    -   `optimizer.py`: Parameter tuning for Z-Score thresholds.
2.  **Execution Layer (C++20):** -   Low-latency execution engine deployed on AWS EC2.
    -   Handles real-time WebSocket streams and order management.

## How to Run
1. Clone the repo.
2. `mkdir build && cd build`
3. `cmake .. && make`
4. `./HFT_Engine`

*Note: This is a portfolio project demonstrating low-latency systems engineering.*
