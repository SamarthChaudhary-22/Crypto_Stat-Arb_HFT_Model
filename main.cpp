#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <numeric>
#include <cmath>
#include <fstream>
#include <map>
#include <cstdint>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include "binance_signer.h"

using json = nlohmann::json;
using namespace std;

// --- CONFIGURATION ---
const string API_KEY = "vF5L3975DpzUexkqHdhMJY4JjNV8VUSljqcWtGjRRlpQJL3G8mYMxnJm3I0ZYKUA";       // <-- PASTE KEYS HERE
const string SECRET_KEY = "fbBCeBmMRoS8Lc1xken4qDqga6lhgTsEzSRV3RshUyj92fhUMrvKhvhMat7GUp9J"; // <-- PASTE KEYS HERE
const string BASE_URL = "https://testnet.binancefuture.com";
const string STRATEGY_FILE = "strategies.json";
const double TRADE_SIZE_USDT = 20.0;

// --- GLOBAL STATE ---
int64_t TIME_OFFSET = 0; // 64-bit Integer to prevent overflow

struct TradingPair {
    string leg1; string leg2; double hedge_ratio;
    int window_size; double entry_z; double exit_z; double stop_z;
    vector<double> spread_history;
    bool in_position = false;
};

// --- PRECISION TIME SYNC ---
void sync_time() {
    cout << "Syncing time with Binance Server..." << endl;
    try {
        // Measure the round-trip
        int64_t t0 = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        cpr::Response r = cpr::Get(cpr::Url{BASE_URL + "/fapi/v1/time"});

        int64_t t1 = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        if (r.status_code == 200) {
            auto j = json::parse(r.text);
            int64_t server_time = j["serverTime"].get<int64_t>(); // <--- 64-BIT FIX

            // Latency is (t1 - t0) / 2
            int64_t latency = (t1 - t0) / 2;

            // Expected Server Time NOW = ServerTime + Latency
            int64_t expected_server_now = server_time + latency;

            // Offset = Expected - Local
            TIME_OFFSET = expected_server_now - t1;

            cout << "   Server Time: " << server_time << endl;
            cout << "   Local Time:  " << t1 << endl;
            cout << "   Latency:     " << latency << "ms" << endl;
            cout << "   Time Offset: " << TIME_OFFSET << "ms" << endl;
        } else {
            cout << "   [FAILED] Could not sync time." << endl;
        }
    } catch (...) {
        cout << "   [ERROR] Connection failed during sync." << endl;
    }
}

int64_t get_current_timestamp() {
    int64_t local_now = chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    return local_now + TIME_OFFSET;
}

void place_order(string symbol, string side, double amount_usdt, double price) {
    if (price <= 0) return;
    int qty = (int)(amount_usdt / price);
    if (qty < 1) qty = 1;

    // 1. Get Timestamp (Corrected)
    // We add +1000ms "Future Padding" to account for internal C++ processing time & upload speed
    int64_t timestamp = get_current_timestamp() - 1000;

    // Note: It sounds counter-intuitive, but setting timestamp slightly in the PAST (relative to server)
    // + a large recvWindow is safer than setting it in the future.
    // If you send a "Future" timestamp, Binance rejects it immediately ("Timestamp > Server Time").
    // We rely on recvWindow to handle the delay.

    string query = "symbol=" + symbol +
                   "&side=" + side +
                   "&type=MARKET" +
                   "&quantity=" + to_string(qty) +
                   "&timestamp=" + to_string(timestamp) +
                   "&recvWindow=60000"; // 60s tolerance

    string signature = BinanceSigner::hmac_sha256(SECRET_KEY, query);
    string payload = query + "&signature=" + signature;

    cout << "   >>> SENDING " << symbol << " " << side << " " << qty << "..." << flush;
    cpr::Response r = cpr::Post(
        cpr::Url{BASE_URL + "/fapi/v1/order"},
        cpr::Body{payload},
        cpr::Header{{"X-MBX-APIKEY", API_KEY}, {"Content-Type", "application/x-www-form-urlencoded"}}
    );

    if (r.status_code == 200) cout << " [SUCCESS]" << endl;
    else cout << " [FAILED] " << r.text << endl;
}

// ... (Rest of load_strategies / get_all_prices / get_z_score / main is identical)
// Just ensure main() calls sync_time() first!

// --- COPY-PASTE UTILS BELOW TO COMPLETE THE FILE ---

map<string, double> get_all_prices() {
    map<string, double> price_map;
    try {
        cpr::Response r = cpr::Get(cpr::Url{BASE_URL + "/fapi/v1/ticker/price"});
        if (r.status_code == 200) {
            auto j = json::parse(r.text);
            for (auto& item : j) {
                price_map[item["symbol"]] = stod(item["price"].get<string>());
            }
        }
    } catch (...) {}
    return price_map;
}

vector<TradingPair> load_strategies() {
    vector<TradingPair> pairs;
    ifstream file(STRATEGY_FILE);
    if (!file.is_open()) return pairs;
    json j; file >> j;
    for (auto& item : j) {
        string s1 = item["leg1"];
        string s2 = item["leg2"];
        if (s1.find("?") != string::npos || s2.find("?") != string::npos) continue;

        TradingPair p;
        p.leg1 = s1; p.leg2 = s2;
        p.hedge_ratio = item["hedge_ratio"];
        p.window_size = (int)item["window_minutes"] * 30;
        p.entry_z = item["entry_z"];
        p.exit_z = item["exit_z"];
        p.stop_z = item.contains("stop_z") ? (double)item["stop_z"] : 6.0;
        pairs.push_back(p);
    }
    return pairs;
}

double get_z_score(const vector<double>& history, double current_val) {
    if (history.empty()) return 0.0;
    double sum = accumulate(history.begin(), history.end(), 0.0);
    double mean = sum / history.size();
    double sq_sum = 0.0;
    for (double v : history) sq_sum += (v - mean) * (v - mean);
    double std_dev = sqrt(sq_sum / history.size());
    return (std_dev == 0) ? 0.0 : (current_val - mean) / std_dev;
}

int main() {
    cout << "--- HFT LIVE ENGINE (64-BIT TIME FIX) ---" << endl;

    sync_time(); // <--- CRITICAL STEP

    vector<TradingPair> portfolio = load_strategies();
    cout << "Loaded " << portfolio.size() << " pairs." << endl;

    int tick = 0;
    while (true) {
        map<string, double> market = get_all_prices();
        if (market.empty()) { this_thread::sleep_for(chrono::seconds(1)); continue; }

        for (auto& pair : portfolio) {
            if (market.find(pair.leg1) == market.end() || market.find(pair.leg2) == market.end()) continue;

            double p1 = market[pair.leg1];
            double p2 = market[pair.leg2];
            double spread = p1 - (pair.hedge_ratio * p2);

            pair.spread_history.push_back(spread);
            if (pair.spread_history.size() > pair.window_size)
                pair.spread_history.erase(pair.spread_history.begin());

            if (pair.spread_history.size() < 20) continue;

            double z = get_z_score(pair.spread_history, spread);

            if (!pair.in_position) {
                if (abs(z) > pair.entry_z) {
                    cout << "\n[ENTRY] " << pair.leg1 << "/" << pair.leg2 << " Z=" << z << endl;
                    if (z > 0) {
                        place_order(pair.leg1, "SELL", TRADE_SIZE_USDT, p1);
                        place_order(pair.leg2, "BUY", TRADE_SIZE_USDT, p2);
                    } else {
                        place_order(pair.leg1, "BUY", TRADE_SIZE_USDT, p1);
                        place_order(pair.leg2, "SELL", TRADE_SIZE_USDT, p2);
                    }
                    pair.in_position = true;
                }
            } else {
                bool take_profit = abs(z) < pair.exit_z;
                bool stop_loss   = abs(z) > pair.stop_z;

                if (take_profit || stop_loss) {
                    cout << "\n[EXIT] " << pair.leg1 << "/" << pair.leg2 << " Z=" << z << endl;
                    place_order(pair.leg1, "BUY", TRADE_SIZE_USDT, p1);
                    place_order(pair.leg2, "SELL", TRADE_SIZE_USDT, p2);
                    pair.in_position = false;
                }
            }
        }

        if (tick % 5 == 0) cout << "\r[Tick " << tick << "] Monitoring..." << flush;
        tick++;
        this_thread::sleep_for(chrono::seconds(2));
    }
    return 0;
}