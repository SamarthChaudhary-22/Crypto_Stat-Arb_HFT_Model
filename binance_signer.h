#ifndef BINANCE_SIGNER_HPP
#define BINANCE_SIGNER_HPP

#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdint>

class BinanceSigner {
private:
    static uint32_t right_rotate(uint32_t val, uint32_t count) {
        return (val >> count) | (val << (32 - count));
    }

    static void sha256_transform(uint32_t* state, const uint8_t* data) {
        uint32_t w[64], a, b, c, d, e, f, g, h;
        uint32_t i, j, t1, t2;
        
        for (i = 0, j = 0; i < 16; i++, j += 4)
            w[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);

        for (; i < 64; i++) {
            t1 = right_rotate(w[i - 15], 7) ^ right_rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            t2 = right_rotate(w[i - 2], 17) ^ right_rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = t1 + w[i - 7] + t2 + w[i - 16];
        }

        a = state[0]; b = state[1]; c = state[2]; d = state[3];
        e = state[4]; f = state[5]; g = state[6]; h = state[7];

        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        for (i = 0; i < 64; i++) {
            t1 = h + (right_rotate(e, 6) ^ right_rotate(e, 11) ^ right_rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            t2 = (right_rotate(a, 2) ^ right_rotate(a, 13) ^ right_rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    static std::string sha256(const std::string& input) {
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        
        std::vector<uint8_t> data(input.begin(), input.end());
        uint64_t bit_len = data.size() * 8;
        data.push_back(0x80);
        while ((data.size() % 64) != 56) data.push_back(0x00);
        
        for (int i = 7; i >= 0; i--) data.push_back((bit_len >> (i * 8)) & 0xFF);

        for (size_t i = 0; i < data.size(); i += 64) sha256_transform(state, &data[i]);

        std::stringstream ss;
        for (int i = 0; i < 8; i++) ss << std::hex << std::setw(8) << std::setfill('0') << state[i];
        return ss.str();
    }

    static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
        std::vector<uint8_t> bytes;
        for (unsigned int i = 0; i < hex.length(); i += 2) {
            std::string byteString = hex.substr(i, 2);
            uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
            bytes.push_back(byte);
        }
        return bytes;
    }

public:
    static std::string hmac_sha256(const std::string& key, const std::string& data) {
        std::vector<uint8_t> k(key.begin(), key.end());
        
        if (k.size() > 64) {
            // If key > 64, hash it
            std::string hashed_key_str = sha256(key);
            k = hex_to_bytes(hashed_key_str); 
        }
        while (k.size() < 64) k.push_back(0x00);

        std::vector<uint8_t> ipad(64), opad(64);
        for (int i = 0; i < 64; i++) {
            ipad[i] = k[i] ^ 0x36;
            opad[i] = k[i] ^ 0x5c;
        }

        std::string inner_data(ipad.begin(), ipad.end());
        inner_data += data;
        std::string inner_hash = sha256(inner_data);
        
        // Convert hex hash back to bytes for outer
        std::vector<uint8_t> inner_hash_bytes = hex_to_bytes(inner_hash);
        std::string outer_data(opad.begin(), opad.end());
        outer_data.append(inner_hash_bytes.begin(), inner_hash_bytes.end());
        
        return sha256(outer_data);
    }
};

// --- THE BRIDGE ---
// This function allows main.cpp to call HMAC_SHA256 directly.
// Note: It swaps the arguments to match the class method: (data, key) -> (key, data)
inline std::string HMAC_SHA256(std::string data, std::string key) {
    return BinanceSigner::hmac_sha256(key, data);
}

#endif
