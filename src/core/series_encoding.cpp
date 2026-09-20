/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "core/series_encoding.h"

#include <cstring>

namespace lgraph {
namespace series {

namespace {

// Portable bit counting: the builtins are not used elsewhere in this code base
// and the MSVC build has no __builtin_clzll.
inline int CountLeadingZeros64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    if ((x & 0xffffffff00000000ULL) == 0) { n += 32; x <<= 32; }
    if ((x & 0xffff000000000000ULL) == 0) { n += 16; x <<= 16; }
    if ((x & 0xff00000000000000ULL) == 0) { n += 8; x <<= 8; }
    if ((x & 0xf000000000000000ULL) == 0) { n += 4; x <<= 4; }
    if ((x & 0xc000000000000000ULL) == 0) { n += 2; x <<= 2; }
    if ((x & 0x8000000000000000ULL) == 0) { ++n; }
    return n;
}

inline int CountTrailingZeros64(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    if ((x & 0x00000000ffffffffULL) == 0) { n += 32; x >>= 32; }
    if ((x & 0x000000000000ffffULL) == 0) { n += 16; x >>= 16; }
    if ((x & 0x00000000000000ffULL) == 0) { n += 8; x >>= 8; }
    if ((x & 0x000000000000000fULL) == 0) { n += 4; x >>= 4; }
    if ((x & 0x0000000000000003ULL) == 0) { n += 2; x >>= 2; }
    if ((x & 0x0000000000000001ULL) == 0) { ++n; }
    return n;
}

inline uint64_t DoubleToBits(double v) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline double BitsToDouble(uint64_t bits) {
    double v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Delta-of-delta field of the timestamp column. See the header for the layout.
// `zz` is already zigzag-encoded.
void WriteTimestampDelta(BitWriter* w, uint64_t zz) {
    if (zz == 0) {
        w->PutBit(0);
    } else if (zz < (1ULL << 7)) {
        w->PutBit(1);
        w->PutBit(0);
        w->WriteBits(zz, 7);
    } else if (zz < (1ULL << 9)) {
        w->PutBit(1);
        w->PutBit(1);
        w->PutBit(0);
        w->WriteBits(zz, 9);
    } else if (zz < (1ULL << 12)) {
        w->PutBit(1);
        w->PutBit(1);
        w->PutBit(1);
        w->PutBit(0);
        w->WriteBits(zz, 12);
    } else {
        w->PutBit(1);
        w->PutBit(1);
        w->PutBit(1);
        w->PutBit(1);
        w->WriteBits(zz, 64);
    }
}

bool ReadTimestampDelta(BitReader* r, uint64_t* zz) {
    uint32_t b0 = 0;
    if (!r->ReadBit(&b0)) return false;
    if (b0 == 0) {
        *zz = 0;
        return true;
    }
    uint32_t b1 = 0;
    if (!r->ReadBit(&b1)) return false;
    uint64_t u = 0;
    if (b1 == 0) {
        if (!r->ReadBits(7, &u)) return false;
    } else {
        uint32_t b2 = 0;
        if (!r->ReadBit(&b2)) return false;
        if (b2 == 0) {
            if (!r->ReadBits(9, &u)) return false;
        } else {
            uint32_t b3 = 0;
            if (!r->ReadBit(&b3)) return false;
            if (b3 == 0) {
                if (!r->ReadBits(12, &u)) return false;
            } else {
                if (!r->ReadBits(64, &u)) return false;
            }
        }
    }
    *zz = u;
    return true;
}

bool EncodeDoubleRaw(const double* values, const uint8_t* nulls, size_t n, std::string* out) {
    out->clear();
    out->reserve(CountNonNull(nulls, n) * sizeof(double));
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) continue;
        const uint64_t bits = DoubleToBits(values[i]);
        for (int b = 0; b < 8; ++b) {
            out->push_back(static_cast<char>((bits >> (b * 8)) & 0xffULL));
        }
    }
    return true;
}

bool DecodeDoubleRaw(const char* data, size_t len, const uint8_t* nulls, size_t n, double* out) {
    if (len < CountNonNull(nulls, n) * sizeof(double)) return false;
    size_t p = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) {
            out[i] = 0;
            continue;
        }
        uint64_t bits = 0;
        for (int b = 0; b < 8; ++b) {
            bits |= static_cast<uint64_t>(static_cast<uint8_t>(data[p + b])) << (b * 8);
        }
        p += 8;
        out[i] = BitsToDouble(bits);
    }
    return true;
}

bool EncodeDoubleGorilla(const double* values, const uint8_t* nulls, size_t n, std::string* out) {
    BitWriter w(CountNonNull(nulls, n) * 8);
    uint64_t prev = 0;
    bool has_prev = false;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) continue;
        const uint64_t bits = DoubleToBits(values[i]);
        if (!has_prev) {
            w.WriteBits(bits, 64);
            prev = bits;
            has_prev = true;
            continue;
        }
        const uint64_t x = bits ^ prev;
        prev = bits;
        if (x == 0) {
            w.PutBit(0);
            continue;
        }
        const int lead = CountLeadingZeros64(x);
        const int trail = CountTrailingZeros64(x);
        const int meaningful = 64 - lead - trail;  // in [1, 64]
        // The set bits of x all lie in [trail, trail + meaningful), so its
        // meaningful bits are all the decoder needs to rebuild x and then bits.
        uint64_t payload = x >> trail;
        if (meaningful < 64) payload &= (1ULL << meaningful) - 1;
        w.PutBit(1);
        w.WriteBits(static_cast<uint64_t>(lead), 6);
        w.WriteBits(static_cast<uint64_t>(meaningful - 1), 6);
        w.WriteBits(payload, meaningful);
    }
    *out = w.Data();
    return true;
}

bool DecodeDoubleGorilla(const char* data, size_t len, const uint8_t* nulls, size_t n,
                         double* out) {
    BitReader r(data, len);
    uint64_t prev = 0;
    bool has_prev = false;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) {
            out[i] = 0;
            continue;
        }
        uint64_t bits = 0;
        if (!has_prev) {
            if (!r.ReadBits(64, &bits)) return false;
            has_prev = true;
        } else {
            uint32_t first = 0;
            if (!r.ReadBit(&first)) return false;
            if (first == 0) {
                bits = prev;
            } else {
                uint64_t lead = 0;
                uint64_t len_minus_one = 0;
                if (!r.ReadBits(6, &lead)) return false;
                if (!r.ReadBits(6, &len_minus_one)) return false;
                const int meaningful = static_cast<int>(len_minus_one) + 1;
                const int trail = 64 - static_cast<int>(lead) - meaningful;
                if (trail < 0) return false;
                uint64_t payload = 0;
                if (!r.ReadBits(meaningful, &payload)) return false;
                bits = prev ^ (payload << trail);
            }
        }
        prev = bits;
        out[i] = BitsToDouble(bits);
    }
    return true;
}

bool EncodeInt64Raw(const int64_t* values, const uint8_t* nulls, size_t n, std::string* out) {
    out->clear();
    out->reserve(CountNonNull(nulls, n) * sizeof(int64_t));
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) continue;
        const uint64_t bits = static_cast<uint64_t>(values[i]);
        for (int b = 0; b < 8; ++b) {
            out->push_back(static_cast<char>((bits >> (b * 8)) & 0xffULL));
        }
    }
    return true;
}

bool DecodeInt64Raw(const char* data, size_t len, const uint8_t* nulls, size_t n, int64_t* out) {
    if (len < CountNonNull(nulls, n) * sizeof(int64_t)) return false;
    size_t p = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) {
            out[i] = 0;
            continue;
        }
        uint64_t bits = 0;
        for (int b = 0; b < 8; ++b) {
            bits |= static_cast<uint64_t>(static_cast<uint8_t>(data[p + b])) << (b * 8);
        }
        p += 8;
        out[i] = static_cast<int64_t>(bits);
    }
    return true;
}

bool EncodeInt64DeltaVarint(const int64_t* values, const uint8_t* nulls, size_t n,
                            std::string* out) {
    out->clear();
    uint64_t prev = 0;
    bool has_prev = false;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) continue;
        const uint64_t cur = static_cast<uint64_t>(values[i]);
        if (!has_prev) {
            AppendVarint(out, ZigZagEncode(values[i]));
            has_prev = true;
        } else {
            // Unsigned subtraction: wraps instead of overflowing, and the bit
            // pattern round-trips exactly.
            AppendVarint(out, ZigZagEncodeBits(cur - prev));
        }
        prev = cur;
    }
    return true;
}

bool DecodeInt64DeltaVarint(const char* data, size_t len, const uint8_t* nulls, size_t n,
                            int64_t* out) {
    const char* p = data;
    const char* end = data + len;
    uint64_t prev = 0;
    bool has_prev = false;
    for (size_t i = 0; i < n; ++i) {
        if (nulls != nullptr && nulls[i] != 0) {
            out[i] = 0;
            continue;
        }
        uint64_t u = 0;
        if (!ReadVarint(&p, end, &u)) return false;
        const uint64_t v = has_prev ? prev + ZigZagDecodeBits(u) : ZigZagDecodeBits(u);
        has_prev = true;
        prev = v;
        out[i] = static_cast<int64_t>(v);
    }
    return true;
}

}  // namespace

const char* ColumnEncodingName(ColumnEncoding enc) {
    switch (enc) {
    case ColumnEncoding::RAW64:
        return "RAW64";
    case ColumnEncoding::GORILLA_XOR:
        return "GORILLA_XOR";
    case ColumnEncoding::ZIGZAG_DELTA_VARINT:
        return "ZIGZAG_DELTA_VARINT";
    }
    return "UNKNOWN";
}

void AppendVarint(std::string* out, uint64_t value) {
    while (value >= 0x80) {
        out->push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<char>(value));
}

bool ReadVarint(const char** p, const char* end, uint64_t* value) {
    uint64_t result = 0;
    int shift = 0;
    const char* q = *p;
    while (q < end) {
        const uint8_t byte = static_cast<uint8_t>(*q++);
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *p = q;
            *value = result;
            return true;
        }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

size_t CountNonNull(const uint8_t* nulls, size_t n) {
    if (nulls == nullptr) return n;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i] == 0) ++count;
    }
    return count;
}

void EncodeNullBitmap(const uint8_t* nulls, size_t n, std::string* out) {
    out->clear();
    out->resize(NullBitmapBytes(n), 0);
    if (nulls == nullptr) return;
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i] != 0) {
            const uint8_t bit = static_cast<uint8_t>(0x80U >> (i % 8));
            (*out)[i / 8] = static_cast<char>(static_cast<uint8_t>((*out)[i / 8]) | bit);
        }
    }
}

bool DecodeNullBitmap(const char* data, size_t len, size_t n, uint8_t* nulls) {
    if (len < NullBitmapBytes(n)) return false;
    for (size_t i = 0; i < n; ++i) {
        nulls[i] = (static_cast<uint8_t>(data[i / 8]) >> (7 - (i % 8))) & 1U;
    }
    return true;
}

bool EncodeTimestampColumn(const int64_t* ts, size_t n, std::string* out) {
    for (size_t i = 1; i < n; ++i) {
        if (ts[i] <= ts[i - 1]) return false;
    }
    BitWriter w(n);
    uint64_t prev_delta = 0;
    for (size_t i = 1; i < n; ++i) {
        // Unsigned subtraction, see ZigZagEncodeBits: an extreme pair of
        // timestamps must not be signed-overflow UB.
        const uint64_t delta =
            static_cast<uint64_t>(ts[i]) - static_cast<uint64_t>(ts[i - 1]);
        WriteTimestampDelta(&w, ZigZagEncodeBits(delta - prev_delta));
        prev_delta = delta;
    }
    *out = w.Data();
    return true;
}

bool DecodeTimestampColumn(const char* data, size_t len, int64_t first_ts, size_t n,
                           int64_t* out) {
    if (n == 0) return false;
    out[0] = first_ts;
    BitReader r(data, len);
    uint64_t prev_delta = 0;
    for (size_t i = 1; i < n; ++i) {
        uint64_t zz = 0;
        if (!ReadTimestampDelta(&r, &zz)) return false;
        const uint64_t delta = prev_delta + ZigZagDecodeBits(zz);
        const int64_t value =
            static_cast<int64_t>(static_cast<uint64_t>(out[i - 1]) + delta);
        if (value <= out[i - 1]) return false;  // corrupt: timestamps must increase
        out[i] = value;
        prev_delta = delta;
    }
    return true;
}

bool EncodeDoubleColumn(const double* values, const uint8_t* nulls, size_t n,
                        ColumnEncoding* encoding, std::string* out) {
    std::string compressed;
    if (!EncodeDoubleGorilla(values, nulls, n, &compressed)) return false;
    const size_t raw_size = CountNonNull(nulls, n) * sizeof(double);
    // Both encodings are always decodable, so pick the smaller one; ties go to
    // RAW64 because it is cheaper to decode.
    if (compressed.size() < raw_size) {
        *encoding = ColumnEncoding::GORILLA_XOR;
        *out = std::move(compressed);
    } else {
        *encoding = ColumnEncoding::RAW64;
        if (!EncodeDoubleRaw(values, nulls, n, out)) return false;
    }
    return true;
}

bool DecodeDoubleColumn(const char* data, size_t len, ColumnEncoding encoding,
                        const uint8_t* nulls, size_t n, double* out) {
    switch (encoding) {
    case ColumnEncoding::RAW64:
        return DecodeDoubleRaw(data, len, nulls, n, out);
    case ColumnEncoding::GORILLA_XOR:
        return DecodeDoubleGorilla(data, len, nulls, n, out);
    case ColumnEncoding::ZIGZAG_DELTA_VARINT:
        return false;  // not a valid encoding for a DOUBLE column
    }
    return false;
}

bool EncodeInt64Column(const int64_t* values, const uint8_t* nulls, size_t n,
                       ColumnEncoding* encoding, std::string* out) {
    std::string compressed;
    if (!EncodeInt64DeltaVarint(values, nulls, n, &compressed)) return false;
    const size_t raw_size = CountNonNull(nulls, n) * sizeof(int64_t);
    if (compressed.size() < raw_size) {
        *encoding = ColumnEncoding::ZIGZAG_DELTA_VARINT;
        *out = std::move(compressed);
    } else {
        *encoding = ColumnEncoding::RAW64;
        if (!EncodeInt64Raw(values, nulls, n, out)) return false;
    }
    return true;
}

bool DecodeInt64Column(const char* data, size_t len, ColumnEncoding encoding,
                       const uint8_t* nulls, size_t n, int64_t* out) {
    switch (encoding) {
    case ColumnEncoding::RAW64:
        return DecodeInt64Raw(data, len, nulls, n, out);
    case ColumnEncoding::ZIGZAG_DELTA_VARINT:
        return DecodeInt64DeltaVarint(data, len, nulls, n, out);
    case ColumnEncoding::GORILLA_XOR:
        return false;  // not a valid encoding for an INT64 column
    }
    return false;
}

}  // namespace series
}  // namespace lgraph
