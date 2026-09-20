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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lgraph {
namespace series {

/**
 * Column encodings used inside a time-series bucket.
 *
 * The code is persisted as one byte per measure column in the bucket value, so
 * existing values must keep decoding to the same meaning. Adding a code is a
 * format change.
 */
enum class ColumnEncoding : uint8_t {
    RAW64 = 0,                // 8 bytes little-endian per non-null value
    GORILLA_XOR = 1,          // doubles: XOR against previous value, bit-packed
    ZIGZAG_DELTA_VARINT = 2,  // int64: zigzag(delta) as a LEB128 varint
};

const char* ColumnEncodingName(ColumnEncoding enc);

/**
 * Bit stream primitives, MSB-first within each field.
 *
 * A field of N bits is written from its highest bit down to its lowest, so a
 * 64-bit field and eight 8-bit fields lay out identically. The final byte of
 * the stream is zero-padded; no field ever spans a byte boundary in the sense
 * that readers only ever ask for bits the writer actually produced.
 */
class BitWriter {
 public:
    BitWriter() = default;
    explicit BitWriter(size_t reserve_bytes) { buf_.reserve(reserve_bytes); }

    void PutBit(uint32_t bit) {
        cur_byte_ = static_cast<uint8_t>(cur_byte_ | ((bit & 1U) << (7 - bits_in_cur_)));
        ++total_bits_;
        if (++bits_in_cur_ == 8) {
            buf_.push_back(static_cast<char>(cur_byte_));
            cur_byte_ = 0;
            bits_in_cur_ = 0;
        }
    }

    // Writes the low `bits` bits of value, most significant bit first.
    // `bits` must be in [0, 64]; bits == 0 writes nothing.
    void WriteBits(uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            PutBit(static_cast<uint32_t>((value >> i) & 1ULL));
        }
    }

    // Total number of bits written so far, including those still in cur_byte_.
    size_t BitSize() const { return total_bits_; }

    const std::string& Data() {
        if (bits_in_cur_ != 0) {
            buf_.push_back(static_cast<char>(cur_byte_));
            cur_byte_ = 0;
            bits_in_cur_ = 0;
        }
        return buf_;
    }

 private:
    std::string buf_;
    uint8_t cur_byte_ = 0;
    int bits_in_cur_ = 0;
    size_t total_bits_ = 0;
};

/**
 * Reader for the stream produced by BitWriter. Every read returns false when
 * the stream is exhausted, which is how truncated values are detected: callers
 * must treat any false as a malformed column rather than as a zero.
 */
class BitReader {
 public:
    BitReader(const char* data, size_t len)
        : data_(reinterpret_cast<const uint8_t*>(data)), len_(len) {}

    bool ReadBit(uint32_t* bit) {
        if (bit_pos_ >= len_ * 8) return false;
        *bit = static_cast<uint32_t>((data_[bit_pos_ >> 3] >> (7 - (bit_pos_ & 7))) & 1U);
        ++bit_pos_;
        return true;
    }

    bool ReadBits(int bits, uint64_t* value) {
        uint64_t v = 0;
        for (int i = 0; i < bits; ++i) {
            uint32_t bit = 0;
            if (!ReadBit(&bit)) return false;
            v = (v << 1) | bit;
        }
        *value = v;
        return true;
    }

    size_t BitPosition() const { return bit_pos_; }
    size_t BitSize() const { return len_ * 8; }

 private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    size_t bit_pos_ = 0;
};

// ---------------------------------------------------------------------------
// Scalar codecs
// ---------------------------------------------------------------------------

inline uint64_t ZigZagEncode(int64_t v) {
    return (static_cast<uint64_t>(v) << 1) ^ (0ULL - (static_cast<uint64_t>(v) >> 63));
}

inline int64_t ZigZagDecode(uint64_t u) {
    return static_cast<int64_t>((u >> 1) ^ (0ULL - (u & 1ULL)));
}

/**
 * The same zigzag map, written for callers that already have the bit pattern
 * of a difference in hand. Deltas of two arbitrary int64 values are computed in
 * unsigned arithmetic so that e.g. a jump from -2^62 to 2^62 wraps instead of
 * being signed-overflow UB; the two's-complement bit pattern round-trips
 * exactly either way.
 */
inline uint64_t ZigZagEncodeBits(uint64_t v) { return (v << 1) ^ (0ULL - (v >> 63)); }

inline uint64_t ZigZagDecodeBits(uint64_t u) { return (u >> 1) ^ (0ULL - (u & 1ULL)); }

// LEB128, least significant 7-bit group first.
void AppendVarint(std::string* out, uint64_t value);

// Reads one varint, advancing *p. Returns false on truncation or on a varint
// longer than 10 bytes (which cannot be a valid uint64).
bool ReadVarint(const char** p, const char* end, uint64_t* value);

// ---------------------------------------------------------------------------
// Null bitmaps
// ---------------------------------------------------------------------------

// Bytes needed for the null bitmap of an n-point column.
inline size_t NullBitmapBytes(size_t n) { return (n + 7) / 8; }

// nulls[i] != 0 marks point i as null. Bit i is the (i%8)-th most significant
// bit of byte i/8, i.e. the same MSB-first convention as BitWriter.
void EncodeNullBitmap(const uint8_t* nulls, size_t n, std::string* out);

bool DecodeNullBitmap(const char* data, size_t len, size_t n, uint8_t* nulls);

// ---------------------------------------------------------------------------
// Timestamp column
// ---------------------------------------------------------------------------

/**
 * Encodes the timestamps of a bucket, in microseconds, exclusive of ts[0]:
 * the bucket header carries ts[0] as `first_ts` because it is also the bucket
 * key. `ts` must be strictly increasing; more than one point may not share a
 * timestamp. Returns false if that precondition is violated.
 *
 * The stream is a delta-of-delta code with the classic gorilla control-bit
 * tree, applied uniformly to every delta (the first delta is a delta against
 * an implicit previous delta of zero):
 *
 *   '0'              -> 0                        (1 bit, the constant-cadence case)
 *   '10'   + 7 bits  -> 1 .. 127
 *   '110'  + 9 bits  -> 128 .. 511
 *   '1110' + 12 bits -> 512 .. 4095
 *   '1111' + 64 bits -> anything larger
 *
 * Values are zigzag-encoded first, so the ranges above are on the zigzag
 * value. The widest escape is 64 bits rather than the 32 bits in the original
 * gorilla paper: timestamps are microseconds here, so a one-day gap is
 * 8.64e10 and does not fit in 32 bits, and a non-representable delta would
 * otherwise corrupt the stream. Constant cadence still costs ~1 bit/point
 * because every repeated cadence is a zero delta-of-delta.
 *
 * The stream occupies 0 bits when n < 2.
 */
bool EncodeTimestampColumn(const int64_t* ts, size_t n, std::string* out);

// `first_ts` must be the bucket header value that was passed to the encoder.
bool DecodeTimestampColumn(const char* data, size_t len, int64_t first_ts, size_t n,
                           int64_t* out);

// ---------------------------------------------------------------------------
// Measure columns
// ---------------------------------------------------------------------------

/**
 * Encodes a DOUBLE measure column. Null points are not written to the stream
 * at all: the reader skips them using the column's null bitmap, so a null
 * never costs compression work (and never needs a placeholder value).
 *
 * GORILLA_XOR layout, over the non-null points only:
 *   - the first non-null value as 64 raw bits, establishing the baseline;
 *   - for every later non-null value: '0' if it is bit-identical to the
 *     previous value, otherwise '1' followed by a 6-bit leading-zero count,
 *     a 6-bit (meaningful bits - 1) and the meaningful bits themselves. The
 *     payload is the XOR of the two IEEE-754 bit patterns with the leading and
 *     trailing zero runs removed.
 *
 * The leading-zero count is 6 bits, not the 5 bits of the original gorilla
 * format: it can be up to 63 when two doubles in the same binade differ only
 * in low mantissa bits, and this implementation has no "reuse the previous
 * window" control pair to escape to.
 *
 * RAW64 layout: 8 bytes little-endian per non-null value.
 *
 * The encoder picks whichever of the two is smaller and reports it through
 * `encoding`; ties go to RAW64, which is cheaper to decode.
 *
 * Decoding needs the same null bitmap, because null points are absent from the
 * stream; out[i] is set to 0 for those points and must be ignored.
 */
bool EncodeDoubleColumn(const double* values, const uint8_t* nulls, size_t n,
                        ColumnEncoding* encoding, std::string* out);

bool DecodeDoubleColumn(const char* data, size_t len, ColumnEncoding encoding,
                        const uint8_t* nulls, size_t n, double* out);

/**
 * Encodes an INT64 measure column (volume, share counts, ...). Null points are
 * skipped, as for doubles.
 *
 * ZIGZAG_DELTA_VARINT layout: the first non-null value as zigzag(varint), then
 * zigzag(value - previous_value) as a varint for each later non-null value.
 * Differences are taken modulo 2^64 (see ZigZagEncodeBits), so values that
 * span the whole int64 range round-trip bit-exactly instead of overflowing.
 *
 * RAW64 layout: 8 bytes little-endian per non-null value.
 *
 * As for doubles, the encoder picks the smaller encoding and decoding needs the
 * null bitmap; out[i] is 0 for null points and must be ignored.
 */
bool EncodeInt64Column(const int64_t* values, const uint8_t* nulls, size_t n,
                       ColumnEncoding* encoding, std::string* out);

bool DecodeInt64Column(const char* data, size_t len, ColumnEncoding encoding,
                       const uint8_t* nulls, size_t n, int64_t* out);

// Number of points in `nulls` that are non-null.
size_t CountNonNull(const uint8_t* nulls, size_t n);

}  // namespace series
}  // namespace lgraph
