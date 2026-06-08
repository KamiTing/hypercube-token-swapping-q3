#include "qk/common.h"

#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>

using namespace std;

namespace qk {

bool PackedStateKey::operator==(const PackedStateKey& other) const {
    return words == other.words;
}

size_t PackedStateKeyHash::operator()(const PackedStateKey& key) const {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (uint64_t word : key.words) {
        h ^= word + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    }
    return static_cast<size_t>(h);
}

uint64_t splitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

bool Fingerprint128::operator==(const Fingerprint128& other) const {
    return low == other.low && high == other.high;
}

size_t Fingerprint128Hash::operator()(const Fingerprint128& key) const {
    return static_cast<size_t>(
        splitmix64(key.low ^ (key.high + 0x9E3779B97F4A7C15ULL))
    );
}

bool fingerprint_less(const Fingerprint128& a, const Fingerprint128& b) {
    return tie(a.high, a.low) < tie(b.high, b.low);
}

bool packed_key_less(const PackedStateKey& a, const PackedStateKey& b) {
    for (size_t word_index = 0; word_index < a.words.size(); ++word_index) {
        uint64_t aw = a.words[word_index];
        uint64_t bw = b.words[word_index];
        if (aw == bw) {
            continue;
        }
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            unsigned av = static_cast<unsigned>((aw >> (byte_index * 8)) & 0xffu);
            unsigned bv = static_cast<unsigned>((bw >> (byte_index * 8)) & 0xffu);
            if (av != bv) {
                return av < bv;
            }
        }
    }
    return false;
}

int node_count(int dim) {
    return 1 << dim;
}

State target_state(int n) {
    State s(n);
    iota(s.begin(), s.end(), 0);
    return s;
}

string key_from_state(const State& s) {
    string key;
    key.reserve(s.size());
    for (int token : s) {
        key.push_back(static_cast<char>(token));
    }
    return key;
}

PackedStateKey packed_key_from_state(const State& s) {
    if (s.size() > 256) {
        throw runtime_error("packed state key supports up to Q8 states only");
    }

    PackedStateKey key;
    for (size_t i = 0; i < s.size(); ++i) {
        uint64_t token = static_cast<uint64_t>(static_cast<unsigned>(s[i]) & 0xffu);
        key.words[i / 8] |= token << ((i % 8) * 8);
    }
    return key;
}

Fingerprint128 fingerprint_component(int position, int token) {
    uint64_t pair = (static_cast<uint64_t>(static_cast<uint32_t>(position)) << 32) |
                    static_cast<uint32_t>(token);
    return {
        splitmix64(pair ^ 0x243F6A8885A308D3ULL),
        splitmix64(pair ^ 0x13198A2E03707344ULL)
    };
}

Fingerprint128 fingerprint_from_state(const State& s) {
    Fingerprint128 fingerprint;
    for (size_t position = 0; position < s.size(); ++position) {
        Fingerprint128 component = fingerprint_component(
            static_cast<int>(position),
            s[position]
        );
        fingerprint.low ^= component.low;
        fingerprint.high ^= component.high;
    }
    return fingerprint;
}

Fingerprint128 fingerprint_after_swap(
    Fingerprint128 fingerprint,
    int u,
    int v,
    int token_u,
    int token_v
) {
    Fingerprint128 old_u = fingerprint_component(u, token_u);
    Fingerprint128 old_v = fingerprint_component(v, token_v);
    Fingerprint128 new_u = fingerprint_component(u, token_v);
    Fingerprint128 new_v = fingerprint_component(v, token_u);
    fingerprint.low ^= old_u.low ^ old_v.low ^ new_u.low ^ new_v.low;
    fingerprint.high ^= old_u.high ^ old_v.high ^ new_u.high ^ new_v.high;
    return fingerprint;
}

} // namespace qk
