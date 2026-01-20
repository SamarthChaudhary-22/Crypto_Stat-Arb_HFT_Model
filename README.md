# Crypto Stat-Arb HFT Engine
A professional-grade High-Frequency Trading (HFT) execution engine written in C++20, designed for statistical arbitrage (pairs trading) on Binance Futures.

This engine is optimized for latency-critical execution, utilizing a multi-threaded producer-consumer architecture and persistent network sessions to achieve single-digit millisecond round-trip times.

# ⚡ Performance Metrics
Logic Latency: ~42 microseconds (Internal tick-to-decision time).

Network Latency (RTT): ~4-7ms (Colocated in AWS Tokyo ap-northeast-1).

Throughput: Capable of processing >10,000 market updates per second without drift.

# 🚀 Key Features
Zero-Copy Market Data: Implemented "Peek-only" shared memory logic to process high-volume WebSocket streams (!bookTicker) without expensive map copying overhead.

Persistent Execution Engine: Dedicated worker thread with cpr::Session keep-alive (TCP/SSL reuse) to eliminate handshake latency.

Thread-Safe Order Queue: Producer-Consumer pattern using std::queue and std::condition_variable to decouple strategy logic from execution IO.

Atomic Position Management: Robust state tracking with "Ceiling Rounding" and minimum notional checks to prevent partial fills and naked positions.

Direct Exchange Signing: Custom header-only HMAC-SHA256 implementation (OpenSSL) for zero-dependency signing.

# 🛠 Tech Stack
Core Language: C++20 (Multi-threading, Atomic types, Mutexes).

#Networking:

ixwebsocket (Real-time Market Data Stream).

libcpr / libcurl (Persistent HTTP Execution).

OpenSSL (Real-time Signature Generation).

Data: nlohmann/json (High-performance parsing).

Infrastructure: AWS EC2 (Amazon Linux 2023) in Tokyo (ap-northeast-1).

# 🧬 Architecture
This project follows a rigorous "Research-to-Production" workflow:

## 1. Research Layer (Python)
data_loader.py: Fetches historical tick/OHLCV data for correlation analysis.

cointegration.py: Identifies mean-reverting pairs using Engle-Granger two-step method.

strategy_optimizer.py: Vectorized backtesting to calibrate Hedge Ratios, Z-Score thresholds, and Halflife.

## 2. Execution Layer (C++)
Main Loop: Zero-copy access to shared market memory; evaluates Z-Scores and OBI (Order Book Imbalance) in <50µs.

Execution Thread: A persistent "hot" thread that waits on a condition variable and fires orders instantly upon signal, skipping DNS/SSL overhead.

# 📥 How to Build & Run
## Prerequisites
C++ Compiler (g++ 11+)

## Libraries: ixwebsocket, openssl, cpr, libcurl, nlohmann_json

## Build Instructions
## 1. Clone the repository
git clone https://github.com/yourusername/HFT-Engine.git
cd HFT-Engine

## 2. Create build directory
mkdir build && cd build

## 3. Compile (Linking all high-performance libs)
g++ -o HFT_Engine main.cpp -I. -lixwebsocket -lssl -lcrypto -lz -lpthread -lcpr -lcurl

## 4. Run (Ensure strategies.json is present)
./HFT_Engine
