#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <set>
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
const double OBI_LONG_THRESHOLD = -0.2;
const double OBI_SHORT_THRESHOLD = 0.2;
const double BET_SIZE = 1000.0; // Global Bet Size ($1000)

// --- SHARED MARKET DATA ---
struct MarketData {
    map<string, double> prices;
    map<string, double> bid_volume;
    map<string, double> ask_volume;
    long long timestamp;
};
MarketData shared_market;
mutex market_mutex;

struct PairConfig {
    string asset1; string asset2;
    double hedge_ratio; double mean; double std_dev;
};

// --- EXECUTION QUEUE (Producer-Consumer) ---
struct OrderRequest {
    string symbol;
    string side;
    double quantity;
};

queue<OrderRequest> order_queue;
mutex queue_mutex;
condition_variable queue_cv;

// --- WORKER 1: EXECUTION THREAD (The Gatling Gun) ---
void ExecutionEngine() {
    cpr::Session session;
    session.SetHeader(cpr::Header{{"X-MBX-APIKEY", API_KEY}});

    // Warmup DNS & SSL
    session.SetUrl(cpr::Url{BASE_URL + "/fapi/v1/time"});
    session.Get();
    cout << "🔫 Execution Engine Ready & Connected." << endl;

    while (true) {
        unique_lock<mutex> lock(queue_mutex);
        queue_cv.wait(lock, []{ return !order_queue.empty(); });

        OrderRequest order = order_queue.front();
        order_queue.pop();
        lock.unlock();

        try {
            // Logic is handled in PlaceOrder, but we cast safely here too
            int qty_int = (int)order.quantity;
            long long timestamp = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
            string query = "symbol=" + order.symbol + "&side=" + order.side + "&type=MARKET&quantity=" + to_string(qty_int) + "&timestamp=" + to_string(timestamp);
            string signature = HMAC_SHA256(query, API_SECRET);
            string url = BASE_URL + "/fapi/v1/order?" + query + "&signature=" + signature;

            session.SetUrl(cpr::Url{url});

            // --- START NETWORK STOPWATCH ---
            auto net_start = chrono::high_resolution_clock::now();

            // FIRE!
            session.Post();

            // --- STOP NETWORK STOPWATCH ---
            auto net_end = chrono::high_resolution_clock::now();
            auto net_duration = chrono::duration_cast<chrono::milliseconds>(net_end - net_start).count();
            long long one_way = net_duration / 2;

            cout << "   🚀 ORDER FIRED: " << order.side << " " << order.symbol
                 << " | RTT: " << net_duration << "ms"
                 << " | Est. Reach: " << one_way << "ms" << endl;

        } catch (...) {}
    }
}

// Helper to push to queue (Safe Rounding Version)
void PlaceOrder(string symbol, string side, double quantity) {
    // 1. Force round UP to the nearest whole number.
    //    Example: 0.57 -> 1.0.  5.2 -> 6.0.
    int qty_int = (int)ceil(quantity);

    // 2. Extra Safety: Even if ceil returns 0 (e.g. qty was 0.0), return.
    if (qty_int <= 0) return;

    {
        lock_guard<mutex> lock(queue_mutex);
        order_queue.push({symbol, side, (double)qty_int});
    }
    queue_cv.notify_one();
}

// --- WORKER 2: WEBSOCKET FEED ---
void WebSocketFeed() {
    ix::WebSocket webSocket;
    webSocket.setUrl(WS_URL);
    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            try {
                json j = json::parse(msg->str);
                lock_guard<mutex> lock(market_mutex);

                if (j.contains("e") && j["e"] == "bookTicker") {
                    string sym = j["s"];
                    shared_market.bid_volume[sym] = stod(string(j["B"]));
                    shared_market.ask_volume[sym] = stod(string(j["A"]));
                }
                if (j.contains("b") && j.contains("a")) {
                    string sym = j["s"];
                    double best_bid = stod(string(j["b"]));
                    double best_ask = stod(string(j["a"]));
                    shared_market.prices[sym] = (best_bid + best_ask) / 2.0;
                }
                shared_market.timestamp = chrono::system_clock::now().time_since_epoch().count();
            } catch (...) {}
        }
    });
    webSocket.start();

    json subscribe_msg;
    subscribe_msg["method"] = "SUBSCRIBE";
    subscribe_msg["params"] = {"!bookTicker"};
    subscribe_msg["id"] = 1;
    this_thread::sleep_for(chrono::seconds(2));
    webSocket.send(subscribe_msg.dump());
    while (true) { this_thread::sleep_for(chrono::seconds(10)); }
}

// --- MAIN LOGIC ---
int main() {
    ix::initNetSystem();
    map<string, int> active_positions;

    cout << "--- HFT ENGINE v6.3 (Safe & Corrected) ---" << endl;

    ifstream f("strategies.json");
    if (!f.good()) { cout << "Error: strategies.json not found!" << endl; return 1; }
    json strat_json = json::parse(f);
    vector<PairConfig> pairs;
    for (auto& item : strat_json) {
        if (!item.contains("leg1")) continue;
        pairs.push_back({item["leg1"], item["leg2"], item["hedge_ratio"], item["mean"], item["std_dev"]});
    }
    cout << "Loaded " << pairs.size() << " pairs." << endl;

    thread ws_thread(WebSocketFeed);
    ws_thread.detach();
    thread exec_thread(ExecutionEngine);
    exec_thread.detach();

    cout << "Waiting for Data..." << endl;
    this_thread::sleep_for(chrono::seconds(3));

    int tick = 0;
    while (true) {
        auto start = chrono::high_resolution_clock::now();

        for (const auto& p : pairs) {

            double p1 = 0, p2 = 0, bid1 = 0, ask1 = 0, bid2 = 0, ask2 = 0;
            bool data_ready = false;

            // --- CRITICAL SECTION: PEEK ONLY (No Copying) ---
            {
                lock_guard<mutex> lock(market_mutex);
                if (shared_market.prices.count(p.asset1) && shared_market.prices.count(p.asset2)) {
                    p1 = shared_market.prices[p.asset1];
                    p2 = shared_market.prices[p.asset2];

                    bid1 = shared_market.bid_volume[p.asset1];
                    ask1 = shared_market.ask_volume[p.asset1];
                    bid2 = shared_market.bid_volume[p.asset2];
                    ask2 = shared_market.ask_volume[p.asset2];

                    data_ready = true;
                }
            }

            if (!data_ready) continue;

            // --- MATH SECTION (Lock-Free) ---
            double spread = log(p1) - (p.hedge_ratio * log(p2));
            double z_score = (spread - p.mean) / p.std_dev;
            string pair_id = p.asset1 + p.asset2;

            double obi1 = 0.0, obi2 = 0.0;
            if (bid1 + ask1 > 0) obi1 = (bid1 - ask1) / (bid1 + ask1);
            if (bid2 + ask2 > 0) obi2 = (bid2 - ask2) / (bid2 + ask2);

            // --- DECISION SECTION ---
            if (active_positions.count(pair_id)) {
                int direction = active_positions[pair_id];

                // --- TAKE PROFIT (Direction -1) ---
                // We entered: SELL P1, BUY P2.
                // We must:    BUY P1, SELL P2.
                if (direction == -1 && z_score < Z_EXIT) {
                    cout << "💰 TAKE PROFIT: " << p.asset1 << "/" << p.asset2 << endl;
                    PlaceOrder(p.asset1, "BUY", BET_SIZE / p1);          // <--- FIXED
                    PlaceOrder(p.asset2, "SELL", (BET_SIZE * p.hedge_ratio) / p2); // <--- FIXED
                    active_positions.erase(pair_id);
                }

                // --- TAKE PROFIT (Direction 1) ---
                // We entered: BUY P1, SELL P2.
                // We must:    SELL P1, BUY P2.
                else if (direction == 1 && z_score > -Z_EXIT) {
                    cout << "💰 TAKE PROFIT: " << p.asset1 << "/" << p.asset2 << endl;
                    PlaceOrder(p.asset1, "SELL", BET_SIZE / p1);         // <--- FIXED
                    PlaceOrder(p.asset2, "BUY", (BET_SIZE * p.hedge_ratio) / p2);  // <--- FIXED
                    active_positions.erase(pair_id);
                }
            }
            else {
                // --- ENTRY (Short Spread) ---
                if (z_score > Z_ENTRY && obi1 < OBI_SHORT_THRESHOLD && obi2 > OBI_LONG_THRESHOLD) {
                    cout << "⚡ SNIPER ENTRY: " << p.asset1 << "/" << p.asset2 << " Z:" << z_score << endl;
                    active_positions[pair_id] = -1;
                    PlaceOrder(p.asset1, "SELL", BET_SIZE / p1);
                    PlaceOrder(p.asset2, "BUY", (BET_SIZE * p.hedge_ratio) / p2);
                }
                // --- ENTRY (Long Spread) ---
                else if (z_score < -Z_ENTRY && obi1 > OBI_LONG_THRESHOLD && obi2 < OBI_SHORT_THRESHOLD) {
                    cout << "⚡ SNIPER ENTRY: " << p.asset1 << "/" << p.asset2 << " Z:" << z_score << endl;
                    active_positions[pair_id] = 1;
                    PlaceOrder(p.asset1, "BUY", BET_SIZE / p1);          // <--- FIXED (Was 100)
                    PlaceOrder(p.asset2, "SELL", (BET_SIZE * p.hedge_ratio) / p2); // <--- FIXED
                }
            }
        }

        auto end = chrono::high_resolution_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
        if (tick++ % 50000 == 0) cout << "[Tick " << tick << "] HFT Latency: " << duration.count() << " us      " << "\r" << flush;
        this_thread::sleep_for(chrono::microseconds(10));
    }
    return 0;
}
