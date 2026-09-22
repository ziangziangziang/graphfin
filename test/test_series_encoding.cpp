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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "./ut_utils.h"
#include "core/series_encoding.h"

namespace {

using lgraph::series::BitReader;
using lgraph::series::BitWriter;
using lgraph::series::ColumnEncoding;
using lgraph::series::CountNonNull;
using lgraph::series::DecodeDoubleColumn;
using lgraph::series::DecodeInt64Column;
using lgraph::series::DecodeNullBitmap;
using lgraph::series::DecodeTimestampColumn;
using lgraph::series::EncodeDoubleColumn;
using lgraph::series::EncodeInt64Column;
using lgraph::series::EncodeNullBitmap;
using lgraph::series::EncodeTimestampColumn;

uint64_t BitsOf(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

constexpr int64_t kUsPerDay = 86400LL * 1000000LL;

std::vector<uint8_t> NoNulls(size_t n) { return std::vector<uint8_t>(n, 0); }

// Builds timestamps from a strictly positive delta list.
std::vector<int64_t> TimestampsFromDeltas(const std::vector<int64_t>& deltas, int64_t start = 0) {
    std::vector<int64_t> ts;
    int64_t cur = start;
    ts.push_back(cur);
    for (int64_t d : deltas) {
        cur += d;
        ts.push_back(cur);
    }
    return ts;
}

void ExpectTimestampRoundTrip(const std::vector<int64_t>& ts) {
    std::string encoded;
    ASSERT_TRUE(EncodeTimestampColumn(ts.data(), ts.size(), &encoded));
    std::vector<int64_t> decoded(ts.size(), 0);
    ASSERT_TRUE(DecodeTimestampColumn(encoded.data(), encoded.size(), ts[0], ts.size(),
                                      decoded.data()));
    EXPECT_EQ(decoded, ts);
}

void ExpectDoubleRoundTrip(const std::vector<double>& values,
                           const std::vector<uint8_t>& nulls,
                           ColumnEncoding* encoding_out = nullptr) {
    ASSERT_EQ(values.size(), nulls.size());
    const size_t n = values.size();
    std::string encoded;
    ColumnEncoding encoding = ColumnEncoding::RAW64;
    ASSERT_TRUE(EncodeDoubleColumn(values.data(), nulls.data(), n, &encoding, &encoded));
    if (encoding_out != nullptr) *encoding_out = encoding;
    std::vector<double> decoded(n, 0);
    ASSERT_TRUE(DecodeDoubleColumn(encoded.data(), encoded.size(), encoding, nulls.data(), n,
                                   decoded.data()));
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i] != 0) continue;
        ASSERT_EQ(BitsOf(decoded[i]), BitsOf(values[i])) << "point " << i;
    }
}

void ExpectInt64RoundTrip(const std::vector<int64_t>& values,
                          const std::vector<uint8_t>& nulls,
                          ColumnEncoding* encoding_out = nullptr) {
    ASSERT_EQ(values.size(), nulls.size());
    const size_t n = values.size();
    std::string encoded;
    ColumnEncoding encoding = ColumnEncoding::RAW64;
    ASSERT_TRUE(EncodeInt64Column(values.data(), nulls.data(), n, &encoding, &encoded));
    if (encoding_out != nullptr) *encoding_out = encoding;
    std::vector<int64_t> decoded(n, 0);
    ASSERT_TRUE(DecodeInt64Column(encoded.data(), encoded.size(), encoding, nulls.data(), n,
                                  decoded.data()));
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i] != 0) continue;
        ASSERT_EQ(decoded[i], values[i]) << "point " << i;
    }
}

}  // namespace

class TestSeriesEncoding : public TuGraphTest {};

TEST_F(TestSeriesEncoding, BitStreamRoundTrip) {
    std::mt19937_64 rng(42);
    for (int width = 0; width <= 64; ++width) {
        const int count = 40;
        std::vector<uint64_t> values;
        BitWriter w;
        for (int i = 0; i < count; ++i) {
            uint64_t v = rng();
            if (width < 64) v &= (1ULL << width) - 1;  // width == 0 would be UB, skip masking
            if (width == 0) v = 0;
            values.push_back(v);
            w.WriteBits(v, width);
        }
        const std::string& data = w.Data();
        EXPECT_EQ(w.BitSize(), static_cast<size_t>(width) * count);
        EXPECT_EQ(data.size(), (w.BitSize() + 7) / 8);
        BitReader r(data.data(), data.size());
        for (int i = 0; i < count; ++i) {
            uint64_t v = 0;
            ASSERT_TRUE(r.ReadBits(width, &v)) << "width " << width << " index " << i;
            EXPECT_EQ(v, values[i]);
        }
    }

    // Single bits and exhaustion.
    BitWriter w;
    w.PutBit(1);
    w.PutBit(0);
    w.PutBit(1);
    const std::string& data = w.Data();
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>(data[0]) >> 5, 0x05);  // 101xxxxx
    BitReader r(data.data(), data.size());
    uint32_t bit = 0;
    EXPECT_TRUE(r.ReadBit(&bit));
    EXPECT_EQ(bit, 1u);
    EXPECT_TRUE(r.ReadBit(&bit));
    EXPECT_EQ(bit, 0u);
    EXPECT_TRUE(r.ReadBit(&bit));
    EXPECT_EQ(bit, 1u);
    // The final byte is zero-padded: the 5 padding bits read as 0 and only a
    // read past the end of the buffer fails.
    uint64_t v = 0;
    EXPECT_TRUE(r.ReadBits(5, &v));
    EXPECT_EQ(v, 0u);
    EXPECT_FALSE(r.ReadBit(&bit));
    EXPECT_FALSE(r.ReadBits(1, &v));
}

TEST_F(TestSeriesEncoding, ZigZagAndVarint) {
    const int64_t values[] = {0, 1, -1, 2, -2, 63, -64, 127, -128,
                              std::numeric_limits<int64_t>::max(),
                              std::numeric_limits<int64_t>::min(), 1LL << 40, -(1LL << 40)};
    for (int64_t v : values) {
        EXPECT_EQ(lgraph::series::ZigZagDecode(lgraph::series::ZigZagEncode(v)), v);
    }
    EXPECT_EQ(lgraph::series::ZigZagEncode(0), 0u);
    EXPECT_EQ(lgraph::series::ZigZagEncode(-1), 1u);
    EXPECT_EQ(lgraph::series::ZigZagEncode(1), 2u);

    const uint64_t varints[] = {0, 1, 127, 128, 300, 16383, 16384, (1ULL << 32),
                                std::numeric_limits<uint64_t>::max()};
    std::string buf;
    for (uint64_t u : varints) lgraph::series::AppendVarint(&buf, u);
    const char* p = buf.data();
    const char* end = buf.data() + buf.size();
    for (uint64_t u : varints) {
        uint64_t out = 0;
        ASSERT_TRUE(lgraph::series::ReadVarint(&p, end, &out));
        EXPECT_EQ(out, u);
    }
    EXPECT_EQ(p, end);

    // Truncated and over-long varints are rejected.
    std::string one;
    lgraph::series::AppendVarint(&one, 300);
    ASSERT_EQ(one.size(), 2u);
    const char* q = one.data();
    uint64_t dummy = 0;
    EXPECT_FALSE(lgraph::series::ReadVarint(&q, one.data() + 1, &dummy));
    std::string overlong(11, static_cast<char>(0x80));
    const char* s = overlong.data();
    EXPECT_FALSE(lgraph::series::ReadVarint(&s, overlong.data() + overlong.size(), &dummy));
}

TEST_F(TestSeriesEncoding, NullBitmapRoundTrip) {
    std::mt19937 rng(7);
    const size_t sizes[] = {1, 7, 8, 9, 63, 64, 65, 1000};
    for (size_t n : sizes) {
        std::vector<uint8_t> nulls(n, 0);
        for (size_t i = 0; i < n; ++i) nulls[i] = static_cast<uint8_t>(rng() % 2);
        std::string bitmap;
        EncodeNullBitmap(nulls.data(), n, &bitmap);
        EXPECT_EQ(bitmap.size(), lgraph::series::NullBitmapBytes(n));
        std::vector<uint8_t> decoded(n, 0);
        ASSERT_TRUE(DecodeNullBitmap(bitmap.data(), bitmap.size(), n, decoded.data()));
        EXPECT_EQ(decoded, nulls);
        if (bitmap.size() > 1) {
            EXPECT_FALSE(DecodeNullBitmap(bitmap.data(), bitmap.size() - 1, n, decoded.data()));
        }
    }
    // A null pointer means "no nulls at all".
    std::string bitmap;
    EncodeNullBitmap(nullptr, 10, &bitmap);
    std::vector<uint8_t> decoded(10, 1);
    ASSERT_TRUE(DecodeNullBitmap(bitmap.data(), bitmap.size(), 10, decoded.data()));
    for (uint8_t v : decoded) EXPECT_EQ(v, 0);
    EXPECT_EQ(CountNonNull(nullptr, 10), 10u);
}

TEST_F(TestSeriesEncoding, TimestampColumn) {
    // One point carries nothing: ts[0] lives in the bucket header.
    std::string encoded;
    ASSERT_TRUE(EncodeTimestampColumn(std::vector<int64_t>{1234}.data(), 1, &encoded));
    EXPECT_TRUE(encoded.empty());

    ExpectTimestampRoundTrip({0, 1});
    ExpectTimestampRoundTrip({-1000, -999, 0, 1});
    ExpectTimestampRoundTrip(TimestampsFromDeltas({1, 1, 1, 1}));

    // Constant daily cadence: ts[0] + 1000 daily bars.
    std::vector<int64_t> daily;
    for (int i = 0; i < 1000; ++i) daily.push_back(i * kUsPerDay);
    ExpectTimestampRoundTrip(daily);
    ASSERT_TRUE(EncodeTimestampColumn(daily.data(), daily.size(), &encoded));
    // First delta escapes to the 64-bit field, the 998 repeats cost 1 bit each.
    EXPECT_LE(encoded.size(), 140u);
    EXPECT_GE(encoded.size(), 120u);

    // Every branch of the control-bit tree, plus a repeat and a negative dod.
    const std::vector<int64_t> widths = TimestampsFromDeltas({63, 163, 1163, 5001163, 5001163,
                                                              5001153});
    ExpectTimestampRoundTrip(widths);

    // Irregular cadence: daily bars with weekends and a suspension gap.
    std::mt19937 rng(11);
    std::vector<int64_t> irregular;
    int64_t cur = 0;
    for (int i = 0; i < 500; ++i) {
        irregular.push_back(cur);
        if (i == 250) {
            cur += 180 * kUsPerDay;  // six-month halt
        } else if (rng() % 5 == 0) {
            cur += 3 * kUsPerDay;  // weekend
        } else {
            cur += kUsPerDay;
        }
    }
    ExpectTimestampRoundTrip(irregular);

    // Microsecond ticks.
    std::vector<int64_t> ticks;
    for (int i = 0; i < 1000; ++i) ticks.push_back(1700000000000000LL + i * 13);
    ExpectTimestampRoundTrip(ticks);

    // Timestamps must be strictly increasing.
    std::vector<int64_t> repeated{10, 10};
    EXPECT_FALSE(EncodeTimestampColumn(repeated.data(), repeated.size(), &encoded));
    std::vector<int64_t> decreasing{10, 9};
    EXPECT_FALSE(EncodeTimestampColumn(decreasing.data(), decreasing.size(), &encoded));
}

TEST_F(TestSeriesEncoding, DoubleColumn) {
    const size_t n = 1000;

    // Constant price: one bit per point after the baseline.
    std::vector<double> constant(n, 12.5);
    ColumnEncoding enc = ColumnEncoding::RAW64;
    ExpectDoubleRoundTrip(constant, NoNulls(n), &enc);
    EXPECT_EQ(enc, ColumnEncoding::GORILLA_XOR);
    std::string encoded;
    ASSERT_TRUE(EncodeDoubleColumn(constant.data(), NoNulls(n).data(), n, &enc, &encoded));
    EXPECT_LE(encoded.size(), 200u);
    EXPECT_GE(encoded.size(), 120u);

    // A real close-price series: two decimals, drifting.
    std::mt19937 rng(3);
    std::vector<double> walk;
    double price = 100.0;
    for (size_t i = 0; i < n; ++i) {
        price += (static_cast<double>(rng() % 201) - 100.0) / 100.0;
        walk.push_back(std::round(price * 100.0) / 100.0);
    }
    ExpectDoubleRoundTrip(walk, NoNulls(n), &enc);
    EXPECT_EQ(enc, ColumnEncoding::GORILLA_XOR);
    ASSERT_TRUE(EncodeDoubleColumn(walk.data(), NoNulls(n).data(), n, &enc, &encoded));
    // Gorilla's win on prices comes from the high bits of consecutive values
    // agreeing, so a real series beats raw 8-byte doubles but not by the
    // margins the timestamp column gets from a constant cadence.
    EXPECT_LT(encoded.size(), n * sizeof(double) * 9 / 10);

    // Incompressible bit patterns fall back to RAW64.
    std::vector<double> random_values;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t bits = (static_cast<uint64_t>(rng()) << 32) | static_cast<uint64_t>(rng());
        double d = 0;
        std::memcpy(&d, &bits, sizeof(d));
        random_values.push_back(d);
    }
    ExpectDoubleRoundTrip(random_values, NoNulls(n), &enc);
    EXPECT_EQ(enc, ColumnEncoding::RAW64);

    // Special values must survive bit-exactly, including NaNs and signed zero.
    std::vector<double> special = {0.0, -0.0, 1.0, -1.0, 1.0,
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::denorm_min(),
                                   std::numeric_limits<double>::max(), 0.0};
    ExpectDoubleRoundTrip(special, NoNulls(special.size()));

    // Null handling: leading null, trailing null, all null, and a single point.
    std::vector<uint8_t> nulls(n, 0);
    for (size_t i = 0; i < n; ++i) nulls[i] = static_cast<uint8_t>(i % 3 == 0);
    ExpectDoubleRoundTrip(walk, nulls);
    std::vector<uint8_t> all_null(n, 1);
    ExpectDoubleRoundTrip(walk, all_null, &enc);
    ASSERT_TRUE(EncodeDoubleColumn(walk.data(), all_null.data(), n, &enc, &encoded));
    EXPECT_TRUE(encoded.empty());
    ExpectDoubleRoundTrip({3.5}, std::vector<uint8_t>{0});
    ExpectDoubleRoundTrip({3.5}, std::vector<uint8_t>{1});
    ExpectDoubleRoundTrip({3.5, 3.5, 4.5}, std::vector<uint8_t>{1, 0, 0});
}

TEST_F(TestSeriesEncoding, Int64Column) {
    const size_t n = 1000;
    ColumnEncoding enc = ColumnEncoding::RAW64;
    std::string encoded;

    // Constant volume: the first value plus one zero delta per later point.
    std::vector<int64_t> constant(n, 1234567);
    ExpectInt64RoundTrip(constant, NoNulls(n), &enc);
    EXPECT_EQ(enc, ColumnEncoding::ZIGZAG_DELTA_VARINT);
    ASSERT_TRUE(EncodeInt64Column(constant.data(), NoNulls(n).data(), n, &enc, &encoded));
    // One byte per zero delta, plus the varint of the first value.
    EXPECT_GE(encoded.size(), n);
    EXPECT_LE(encoded.size(), n + 4);

    std::mt19937 rng(5);
    std::vector<int64_t> volumes;
    for (size_t i = 0; i < n; ++i) volumes.push_back(rng() % 5000000 - 1000);
    ExpectInt64RoundTrip(volumes, NoNulls(n), &enc);
    EXPECT_EQ(enc, ColumnEncoding::ZIGZAG_DELTA_VARINT);

    // Large magnitudes and negative values. The jump between -2^62 and 2^62
    // overflows a signed difference, so this also pins the wrapping contract.
    std::vector<int64_t> big = {0, -(1LL << 62), (1LL << 62), -(1LL << 40), 1LL << 40, 0,
                                std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max()};
    ExpectInt64RoundTrip(big, NoNulls(big.size()));

    // Nulls are skipped entirely; all-null columns cost nothing.
    std::vector<uint8_t> nulls(n, 0);
    for (size_t i = 0; i < n; ++i) nulls[i] = static_cast<uint8_t>(i % 4 == 1);
    ExpectInt64RoundTrip(volumes, nulls, &enc);
    std::vector<uint8_t> all_null(n, 1);
    ASSERT_TRUE(EncodeInt64Column(volumes.data(), all_null.data(), n, &enc, &encoded));
    EXPECT_TRUE(encoded.empty());
    EXPECT_EQ(enc, ColumnEncoding::RAW64);

    ExpectInt64RoundTrip({7}, std::vector<uint8_t>{1});
    ExpectInt64RoundTrip({7, -7}, std::vector<uint8_t>{1, 0});
}

TEST_F(TestSeriesEncoding, RejectsTruncatedAndMismatchedInput) {
    std::mt19937 rng(13);
    std::vector<int64_t> ts;
    std::vector<double> closes;
    std::vector<int64_t> volumes;
    for (int i = 0; i < 200; ++i) {
        ts.push_back(i * kUsPerDay);
        closes.push_back(100.0 + static_cast<double>(rng() % 100));
        volumes.push_back(rng() % 1000000);
    }
    const std::vector<uint8_t> nulls = NoNulls(ts.size());

    std::string ts_encoded;
    ASSERT_TRUE(EncodeTimestampColumn(ts.data(), ts.size(), &ts_encoded));
    ASSERT_GT(ts_encoded.size(), 4u);
    std::vector<int64_t> ts_out(ts.size(), 0);
    EXPECT_FALSE(DecodeTimestampColumn(ts_encoded.data(), ts_encoded.size() / 2, ts[0], ts.size(),
                                       ts_out.data()));

    ColumnEncoding enc = ColumnEncoding::RAW64;
    std::string dbl_encoded;
    ASSERT_TRUE(EncodeDoubleColumn(closes.data(), nulls.data(), closes.size(), &enc,
                                   &dbl_encoded));
    ASSERT_GT(dbl_encoded.size(), 4u);
    std::vector<double> dbl_out(closes.size(), 0);
    EXPECT_FALSE(DecodeDoubleColumn(dbl_encoded.data(), dbl_encoded.size() / 2, enc,
                                    nulls.data(), closes.size(), dbl_out.data()));
    // An encoding code belonging to the other column kind is a malformed value.
    EXPECT_FALSE(DecodeDoubleColumn(dbl_encoded.data(), dbl_encoded.size(),
                                    ColumnEncoding::ZIGZAG_DELTA_VARINT, nulls.data(),
                                    closes.size(), dbl_out.data()));

    std::string int_encoded;
    ASSERT_TRUE(EncodeInt64Column(volumes.data(), nulls.data(), volumes.size(), &enc,
                                  &int_encoded));
    ASSERT_GT(int_encoded.size(), 4u);
    std::vector<int64_t> int_out(volumes.size(), 0);
    EXPECT_FALSE(DecodeInt64Column(int_encoded.data(), int_encoded.size() / 2, enc, nulls.data(),
                                   volumes.size(), int_out.data()));
    EXPECT_FALSE(DecodeInt64Column(int_encoded.data(), int_encoded.size(),
                                   ColumnEncoding::GORILLA_XOR, nulls.data(), volumes.size(),
                                   int_out.data()));

    // A hand-built timestamp stream whose second delta is negative must be
    // rejected rather than producing a non-monotonic column.
    BitWriter w;
    // '1111' + 64 bits = zigzag(-1) = 1, i.e. a delta of -1.
    w.PutBit(1);
    w.PutBit(1);
    w.PutBit(1);
    w.PutBit(1);
    w.WriteBits(1, 64);
    const std::string& crafted = w.Data();
    int64_t out[2] = {0, 0};
    EXPECT_FALSE(DecodeTimestampColumn(crafted.data(), crafted.size(), 1000, 2, out));

    EXPECT_STREQ(lgraph::series::ColumnEncodingName(ColumnEncoding::RAW64), "RAW64");
    EXPECT_STREQ(lgraph::series::ColumnEncodingName(ColumnEncoding::GORILLA_XOR), "GORILLA_XOR");
    EXPECT_STREQ(lgraph::series::ColumnEncodingName(ColumnEncoding::ZIGZAG_DELTA_VARINT),
                 "ZIGZAG_DELTA_VARINT");
}

TEST_F(TestSeriesEncoding, FuzzRoundTripIsByteStable) {
    std::mt19937_64 rng(20240920);
    for (int iter = 0; iter < 300; ++iter) {
        const size_t n = 1 + static_cast<size_t>(rng() % 1000);
        const uint32_t null_rate = static_cast<uint32_t>(rng() % 40);
        std::vector<int64_t> ts(n, 0);
        std::vector<double> values(n, 0);
        std::vector<int64_t> ints(n, 0);
        std::vector<uint8_t> nulls(n, 0);
        int64_t cur = static_cast<int64_t>(rng() % 1000000);
        double price = 10.0 + static_cast<double>(rng() % 1000) / 100.0;
        int64_t volume = static_cast<int64_t>(rng() % 100000);
        for (size_t i = 0; i < n; ++i) {
            ts[i] = cur;
            // Mix of minute, daily and long gaps, plus microsecond noise.
            const uint32_t mode = static_cast<uint32_t>(rng() % 4);
            cur += (mode == 0) ? 60000000LL
                               : (mode == 1) ? kUsPerDay
                                             : (mode == 2) ? 1 + static_cast<int64_t>(rng() % 100)
                                                           : 30 * kUsPerDay;
            const bool smooth = (rng() % 2) == 0;
            if (smooth) {
                price += (static_cast<double>(rng() % 2001) - 1000.0) / 5000.0;
                values[i] = price;
            } else {
                const uint64_t bits = (rng() << 32) | (rng() & 0xffffffffULL);
                double noise = 0;
                std::memcpy(&noise, &bits, sizeof(noise));
                values[i] = noise;
            }
            volume += static_cast<int64_t>(rng() % 200000) - 100000;
            ints[i] = volume;
            nulls[i] = (rng() % 100) < null_rate ? 1 : 0;
        }

        // Timestamps: exact round trip, and re-encoding the decoded column
        // reproduces the identical bytes.
        std::string ts_encoded;
        ASSERT_TRUE(EncodeTimestampColumn(ts.data(), n, &ts_encoded)) << "iter " << iter;
        std::vector<int64_t> ts_decoded(n, 0);
        ASSERT_TRUE(DecodeTimestampColumn(ts_encoded.data(), ts_encoded.size(), ts[0], n,
                                          ts_decoded.data())) << "iter " << iter;
        ASSERT_EQ(ts_decoded, ts) << "iter " << iter;
        std::string ts_again;
        ASSERT_TRUE(EncodeTimestampColumn(ts_decoded.data(), n, &ts_again));
        EXPECT_EQ(ts_again, ts_encoded) << "iter " << iter;

        ColumnEncoding enc = ColumnEncoding::RAW64;
        std::string dbl_encoded;
        ASSERT_TRUE(EncodeDoubleColumn(values.data(), nulls.data(), n, &enc, &dbl_encoded));
        std::vector<double> dbl_decoded(n, 0);
        ASSERT_TRUE(DecodeDoubleColumn(dbl_encoded.data(), dbl_encoded.size(), enc, nulls.data(),
                                       n, dbl_decoded.data())) << "iter " << iter;
        std::string dbl_again;
        ColumnEncoding enc_again = ColumnEncoding::RAW64;
        ASSERT_TRUE(EncodeDoubleColumn(dbl_decoded.data(), nulls.data(), n, &enc_again,
                                       &dbl_again));
        EXPECT_EQ(enc_again, enc) << "iter " << iter;
        EXPECT_EQ(dbl_again, dbl_encoded) << "iter " << iter;
        for (size_t i = 0; i < n; ++i) {
            if (nulls[i] != 0) continue;
            ASSERT_EQ(BitsOf(dbl_decoded[i]), BitsOf(values[i])) << "iter " << iter << " i " << i;
        }

        std::string int_encoded;
        ASSERT_TRUE(EncodeInt64Column(ints.data(), nulls.data(), n, &enc, &int_encoded));
        std::vector<int64_t> int_decoded(n, 0);
        ASSERT_TRUE(DecodeInt64Column(int_encoded.data(), int_encoded.size(), enc, nulls.data(),
                                      n, int_decoded.data())) << "iter " << iter;
        std::string int_again;
        ASSERT_TRUE(EncodeInt64Column(int_decoded.data(), nulls.data(), n, &enc_again,
                                      &int_again));
        EXPECT_EQ(int_again, int_encoded) << "iter " << iter;
        for (size_t i = 0; i < n; ++i) {
            if (nulls[i] != 0) continue;
            ASSERT_EQ(int_decoded[i], ints[i]) << "iter " << iter << " i " << i;
        }
    }
}
