#include "modelforge/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mf {

namespace {
inline uint32_t rotl32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

inline uint32_t rotr32(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string hexFromBytes(const uint8_t* data, size_t length) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return out;
}
} // namespace

std::string base64Encode(const std::string& data) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c : data) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(buffer << (6 - bits)) & 0x3f]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64Decode(const std::string& data) {
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buffer = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int value = valueOf(c);
        if (value < 0) return std::string();
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string sha1Raw(const std::string& data) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string message = data;
    const uint64_t originalBits = static_cast<uint64_t>(message.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back(static_cast<char>(0));
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>((originalBits >> (i * 8)) & 0xff));

    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint8_t digest[20];
    const uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(hs[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(hs[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(hs[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(hs[i]);
    }
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string sha1(const std::string& data) {
    const std::string raw = sha1Raw(data);
    return hexFromBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

Sha256::Sha256() : bitLength_(0), bufferLength_(0) {
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85; state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c; state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::transform(const uint8_t* chunk) {
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
    uint32_t w[64]{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
               (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, size_t length) {
    const uint8_t* input = static_cast<const uint8_t*>(data);
    bitLength_ += static_cast<uint64_t>(length) * 8;
    while (length > 0) {
        const size_t take = std::min(length, sizeof(buffer_) - bufferLength_);
        std::memcpy(buffer_ + bufferLength_, input, take);
        bufferLength_ += take;
        input += take;
        length -= take;
        if (bufferLength_ == sizeof(buffer_)) {
            transform(buffer_);
            bufferLength_ = 0;
        }
    }
}

std::string Sha256::hexDigest() {
    const uint64_t bits = bitLength_;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (bufferLength_ != 56) update(&zero, 1);
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    bitLength_ -= 64 * 8;
    update(lengthBytes, sizeof(lengthBytes));
    uint8_t digest[32];
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return hexFromBytes(digest, sizeof(digest));
}

std::string sha256File(const ghc::filesystem::path& path,
                       const std::function<void(uint64_t, uint64_t)>& progress) {
    std::ifstream input(path.u8string(), std::ios::binary);
    if (!input) throw std::runtime_error("无法打开文件计算 SHA256: " + path.u8string());
    input.seekg(0, std::ios::end);
    const uint64_t total = static_cast<uint64_t>(input.tellg());
    input.seekg(0, std::ios::beg);
    Sha256 hash;
    std::vector<char> buffer(4 * 1024 * 1024);
    uint64_t done = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        hash.update(buffer.data(), static_cast<size_t>(count));
        done += static_cast<uint64_t>(count);
        if (progress) progress(done, total);
    }
    if (!input.eof() && input.fail()) throw std::runtime_error("读取文件失败: " + path.u8string());
    return hash.hexDigest();
}

} // namespace mf
