#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <iomanip>
#include <sstream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXUserAgent.h>
#include "binance_signer.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;

// --- CONFIGURATION ---
const string API_KEY = std::getenv("BINANCE_API_KEY") ? std::getenv("BINANCE_API_KEY") : "YOUR_KEY_HERE";
const string API_SECRET = std::getenv("BINANCE_API_SECRET") ? std::getenv("BINANCE_API_SECRET") : "YOUR_SECRET_HERE";
const bool IS_TESTNET = true;
const string BASE_URL = IS_TESTNET ? "https://testnet.binancefuture.com" : "https://fapi.binance.com";
const string WS_URL = IS_TESTNET ? "wss://stream.binancefuture.com/ws" : "wss://fstream.binance.com/ws";

// --- STRATEGY PARAMETERS ---
const double Z_ENTRY = 2.0;
const double Z_EXIT = 0.5;
const double MAX_SAFE_Z = 25.0;
const double BET_SIZE = 1000.0;
const double OBI_LONG_THRESHOLD = -0.2;
const double OBI_SHORT_THRESHOLD = 0.2;

// --- SHARED DATA ---
struct MarketData {
    map<string, double> prices;
    map<string, double> bid_volume;
    map<string, double> ask_volume;
};
MarketData shared_market;
mutex market_mutex;

struct PairConfig {
    string asset1; string asset2;
    double hedge_ratio; double mean; double std_dev;
};
vector<PairConfig> global_pairs;

// --- PRECISION MAPPING ---
map<string, int> symbol_precision;

// --- EXECUTION QUEUE ---
struct OrderRequest {
    string symbol;
    string side;
    double quantity;
    bool is_close;
};
deque<OrderRequest> order_queue;
mutex queue_mutex;
condition_variable queue_cv;
atomic<bool> GLOBAL_HALT(false);

// --- FORWARD DECLARATION ---
void PlaceOrder(string symbol, string side, double quantity, bool is_close = false);

// --- HELPER: EXACT ROUNDING ---
string FormatQuantity(string symbol, double quantity) {
    int decimals = 0;
    if (symbol_precision.count(symbol)) {
        decimals = symbol_precision[symbol];
    } else {
        if (quantity > 1000) decimals = 0;
        else if (quantity > 1.0) decimals = 1;
        else decimals = 3;
    }
    double multiplier = pow(10.0, decimals);
    double rounded = floor(quantity * multiplier) / multiplier;
    stringstream stream;
    stream << fixed << setprecision(decimals) << rounded;
    return stream.str();
}

// --- HELPER: SAFE JSON ---
double SafeGetDouble(const json& j, const string& key) {
    if (j.contains(key) && !j[key].is_null()) {
        if (j[key].is_number()) return j[key].get<double>();
        if (j[key].is_string()) return stod(j[key].get<string>());
    }
    return 0.0;
}

// --- INITIALIZATION ---
void LoadExchangeInfo() {
    cout << "Fetching Exchange Precision Rules..." << endl;
    cpr::Session session;
    session.SetUrl(cpr::Url{BASE_URL + "/fapi/v1/exchangeInfo"});
    auto response = session.Get();
    if (response.status_code == 200) {
        try {
            json j = json::parse(response.text);
            for (auto& s : j["symbols"]) {
                string sym = s["symbol"];
                for (auto& f : s["filters"]) {
                    if (f["filterType"] == "LOT_SIZE") {
                        double stepSize = stod(string(f["stepSize"]));
                        int precision = 0;
                        if (stepSize < 1.0) precision = (int)round(-log10(stepSize));
                        symbol_precision[sym] = precision;
                        break;
                    }
                }
            }
            cout << " Loaded precision for " << symbol_precision.size() << " symbols." << endl;
        } catch (...) { cout << " Error parsing Exchange Info." << endl; }
    }
}

// --- WORKER 1: EXECUTION THREAD ---
void ExecutionEngine() {
    cpr::Session session;
    session.SetHeader(cpr::Header{{"X-MBX-APIKEY", API_KEY}});

    while (true) {
        unique_lock<mutex> lock(queue_mutex);
        queue_cv.wait(lock, []{ return !order_queue.empty(); });

        OrderRequest order = order_queue.front();
        order_queue.pop_front();
        lock.unlock();

        try {
            long long timestamp = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();

            // USE PRECISE FORMATTER
            string qty_str = FormatQuantity(order.symbol, order.quantity);

            string query = "symbol=" + order.symbol + "&side=" + order.side + "&type=MARKET&quantity=" + qty_str + "&timestamp=" + to_string(timestamp);
            if (order.is_close) query += "&reduceOnly=true";

            string signature = HMAC_SHA256(query, API_SECRET);
            string url = BASE_URL + "/fapi/v1/order?" + query + "&signature=" + signature;

            session.SetUrl(cpr::Url{url});

            // MEASURE NETWORK SPEED
            auto net_start = chrono::high_resolution_clock::now();
            auto response = session.Post();
            auto net_end = chrono::high_resolution_clock::now();

            long long rtt_us = chrono::duration_cast<chrono::microseconds>(net_end - net_start).count();
            double rtt_ms = rtt_us / 1000.0;

            if (response.status_code == 200) {
                json j = json::parse(response.text);
                long long transact_time = j["transactTime"];
                long long latency_to_server = transact_time - timestamp;

                cout << (order.is_close ? "CLOSE " : " ENTRY ") << order.symbol << " [FILLED]" << endl;
                cout << "      ├─  Network RTT:   " << rtt_ms << " ms" << endl;
                cout << "      ├─  Binance Time:  " << transact_time << endl;
                cout << "      └─ Diff (Local->Remote): " << latency_to_server << " ms" << endl;
            } else {
                cout << "❌ FAILED (" << order.symbol << "): " << response.text << endl;
            }
        } catch (...) {}
    }
}

// --- QUEUE LOGIC ---
void PlaceOrder(string symbol, string side, double quantity, bool is_close) {
    if (quantity <= 0) return;
    {
        lock_guard<mutex> lock(queue_mutex);
        if (is_close) {
             order_queue.push_front({symbol, side, quantity, is_close});
             cout << " URGENT: " << symbol << " Close Order JUMPING QUEUE!" << endl;
        } else {
             order_queue.push_back({symbol, side, quantity, is_close});
        }
    }
    queue_cv.notify_one();
}

// --- WORKER 3: RISK ENGINE ---
void RiskEngine() {
    cpr::Session session;
    session.SetHeader(cpr::Header{{"X-MBX-APIKEY", API_KEY}});

    const double MAX_LOSS_PER_POS = -20.0;
    const double GLOBAL_PNL_KILL = -100.0;

    cout << "Risk Engine Active." << endl;
    int heartbeat = 0;

    while (true) {
        try {
            if (heartbeat++ % 5 == 0) cout << "Risk Engine Scanning..." << endl;

            long long timestamp = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
            string query = "timestamp=" + to_string(timestamp);
            string signature = HMAC_SHA256(query, API_SECRET);
            string url = BASE_URL + "/fapi/v2/positionRisk?" + query + "&signature=" + signature;

            session.SetUrl(cpr::Url{url});
            auto response = session.Get();

            if (response.status_code == 200) {
                json j_positions = json::parse(response.text);
                double total_unrealized_pnl = 0.0;

                for (auto& pos : j_positions) {
                    string symbol = pos["symbol"];
                    double amt = SafeGetDouble(pos, "positionAmt");
                    double pnl = SafeGetDouble(pos, "unrealizedProfit");
                    total_unrealized_pnl += pnl;

                    if (abs(amt) > 0.0) {
                        if (pnl < MAX_LOSS_PER_POS) {
                            cout << "STOP LOSS TRIGGERED: " << symbol << " PnL: $" << pnl << endl;
                            string side = (amt > 0) ? "SELL" : "BUY";
                            PlaceOrder(symbol, side, abs(amt), true);
                        }
                    }
                }

                if (total_unrealized_pnl < GLOBAL_PNL_KILL && !GLOBAL_HALT) {
                     cout << "GLOBAL KILL SWITCH TRIGGERED" << endl;
                     GLOBAL_HALT = true;
                     for (auto& pos : j_positions) {
                        double amt = SafeGetDouble(pos, "positionAmt");
                        string sym = pos["symbol"];
                        if (abs(amt) > 0.0) {
                            string side = (amt > 0) ? "SELL" : "BUY";
                            PlaceOrder(sym, side, abs(amt), true);
                        }
                     }
                }
            }
        } catch (const exception& e) {
             cout << "RISK ENGINE CRASHED: " << e.what() << endl;
        }
        this_thread::sleep_for(chrono::seconds(1));
    }
}

// --- WEBSOCKET (WITH MICROPRICING) ---
void WebSocketFeed() {
    ix::WebSocket webSocket;
    webSocket.setUrl(WS_URL);
    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            try {
                json j = json::parse(msg->str);
                lock_guard<mutex> lock(market_mutex);
                if (j.contains("b") && j.contains("a")) {
                    string sym = j["s"];
                    double bb = stod(string(j["b"])); // Best Bid
                    double ba = stod(string(j["a"])); // Best Ask
                    double bv = stod(string(j["B"])); // Bid Vol
                    double av = stod(string(j["A"])); // Ask Vol

                    // --- MICROPRICING LOGIC ---
                    // Weighted Mid-Price = (BidPrice * AskVol + AskPrice * BidVol) / (TotalVol)
                    double weighted_price = (bb * av + ba * bv) / (bv + av);

                    shared_market.prices[sym] = weighted_price; // <--- UPDATED
                    shared_market.bid_volume[sym] = bv;
                    shared_market.ask_volume[sym] = av;
                }
            } catch (...) {}
        }
    });
    webSocket.start();
    json sub;
    sub["method"] = "SUBSCRIBE";
    sub["params"] = {"!bookTicker"};
    sub["id"] = 1;
    this_thread::sleep_for(chrono::seconds(1));
    webSocket.send(sub.dump());
    while (true) this_thread::sleep_for(chrono::seconds(10));
}

// --- MAIN ---
int main() {
    ix::initNetSystem();
    cout << "--- HFT ENGINE v7.7 (True Micropricing) ---" << endl;

    LoadExchangeInfo();

    ifstream f("strategies.json");
    if (!f.good()) return 1;
    json strat_json = json::parse(f);
    for (auto& item : strat_json) {
        if (!item.contains("leg1")) continue;
        global_pairs.push_back({item["leg1"], item["leg2"], item["hedge_ratio"], item["mean"], item["std_dev"]});
    }

    thread ws_thread(WebSocketFeed);
    ws_thread.detach();
    thread exec_thread(ExecutionEngine);
    exec_thread.detach();
    thread risk_thread(RiskEngine);
    risk_thread.detach();

    cout << "Waiting for Data..." << endl;
    this_thread::sleep_for(chrono::seconds(3));

    map<string, int> active_positions;
	int tick = 0;

    while (true) {
        if (GLOBAL_HALT) { this_thread::sleep_for(chrono::seconds(1)); continue; }
		auto start = chrono::high_resolution_clock::now();
        for (const auto& p : global_pairs) {
            double p1=0, p2=0, b1=0, a1=0, b2=0, a2=0;
            {
                lock_guard<mutex> lock(market_mutex);
                if (shared_market.prices.count(p.asset1) && shared_market.prices.count(p.asset2)) {
                    p1 = shared_market.prices[p.asset1]; // Now contains Microprice
                    p2 = shared_market.prices[p.asset2]; // Now contains Microprice
                    b1 = shared_market.bid_volume[p.asset1];
                    a1 = shared_market.ask_volume[p.asset1];
                    b2 = shared_market.bid_volume[p.asset2];
                    a2 = shared_market.ask_volume[p.asset2];
                }
            }
            if (p1 == 0 || p2 == 0) continue;

            double spread = log(p1) - (p.hedge_ratio * log(p2));
            double z_score = (spread - p.mean) / p.std_dev;
            string pair_id = p.asset1 + p.asset2;

            double obi1 = 0.0, obi2 = 0.0;
            if (b1 + a1 > 0) obi1 = (b1 - a1) / (b1 + a1);
            if (b2 + a2 > 0) obi2 = (b2 - a2) / (b2 + a2);

            if (abs(z_score) > MAX_SAFE_Z) continue;

            if (active_positions.count(pair_id)) {
                int dir = active_positions[pair_id];
                if ((dir == -1 && z_score < Z_EXIT) || (dir == 1 && z_score > -Z_EXIT)) {
                    cout << " TAKE PROFIT: " << pair_id << endl;
                    PlaceOrder(p.asset1, dir == -1 ? "BUY" : "SELL", BET_SIZE / p1, true);
                    PlaceOrder(p.asset2, dir == -1 ? "SELL" : "BUY", (BET_SIZE * p.hedge_ratio) / p2, true);
                    active_positions.erase(pair_id);
                    this_thread::sleep_for(chrono::milliseconds(100));
                }
            } else {
                if (z_score > Z_ENTRY && obi1 < OBI_SHORT_THRESHOLD && obi2 > OBI_LONG_THRESHOLD) {
                    cout << "LONG ENTRY " << pair_id << " Z:" << z_score << endl;
                    active_positions[pair_id] = -1;
                    PlaceOrder(p.asset1, "SELL", BET_SIZE / p1);
                    PlaceOrder(p.asset2, "BUY", (BET_SIZE * p.hedge_ratio) / p2);
                    this_thread::sleep_for(chrono::milliseconds(200));
                } else if (z_score < -Z_ENTRY && obi1 > OBI_LONG_THRESHOLD && obi2 < OBI_SHORT_THRESHOLD) {
                    cout << "LONG ENTRY " << pair_id << " Z:" << z_score << endl;
                    active_positions[pair_id] = 1;
                    PlaceOrder(p.asset1, "BUY", BET_SIZE / p1);
                    PlaceOrder(p.asset2, "SELL", (BET_SIZE * p.hedge_ratio) / p2);
                    this_thread::sleep_for(chrono::milliseconds(200));
                }
            }
        }
        auto end = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
        if (tick++ % 50000 == 0) cout << "[Tick " << tick << "] HFT Latency: " << duration.count() << " us      " << "\r" << flush;
        this_thread::sleep_for(chrono::microseconds(100));
    }
    return 0;
}
