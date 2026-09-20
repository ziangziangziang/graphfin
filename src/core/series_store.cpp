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

#include "core/series_store.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "fma-common/string_util.h"

#include "core/data_type.h"
#include "core/defs.h"
#include "core/series_encoding.h"

namespace lgraph {
namespace series {

namespace {

const uint32_t kBucketMagic = 0x31534254;  // "TSB1" as little-endian bytes
const uint16_t kBucketFormatVersion = 1;
const uint16_t kFlagHasNulls = 1;

const uint8_t kKeyKindVertex = 0x00;
const uint8_t kKeyKindEdge = 0x01;

// Key sizes must match the layout documented in the header.
const size_t kVertexElementPrefixSize = 1 + 5;                    // kind + vid
const size_t kEdgeElementPrefixSize = 1 + 5 + 2 + 8 + 5 + 4;      // kind + euid
const size_t kFieldIdSize = 2;
const size_t kBucketStartSize = 8;

// ---------------------------------------------------------------------------
// Little-endian scalars, used inside bucket values.
// ---------------------------------------------------------------------------

void AppendLeU16(std::string* out, uint16_t v) {
    out->push_back(static_cast<char>(v & 0xffu));
    out->push_back(static_cast<char>((v >> 8) & 0xffu));
}

void AppendLeU32(std::string* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}

void AppendLeI64(std::string* out, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((u >> (8 * i)) & 0xffu));
}

uint16_t ReadLeU16(const char* p) {
    return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) |
                                 (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8));
}

uint32_t ReadLeU32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

int64_t ReadLeI64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    }
    return static_cast<int64_t>(v);
}

// Bounds-checked cursor over a bucket value.
struct ByteReader {
    const char* p = nullptr;
    const char* end = nullptr;

    ByteReader(const char* begin, size_t len) : p(begin), end(begin + len) {}

    size_t Remaining() const { return static_cast<size_t>(end - p); }

    bool Take(size_t n, const char** out) {
        if (Remaining() < n) return false;
        *out = p;
        p += n;
        return true;
    }
    bool ReadU8(uint8_t* v) {
        const char* q = nullptr;
        if (!Take(1, &q)) return false;
        *v = static_cast<uint8_t>(*q);
        return true;
    }
    bool ReadU16(uint16_t* v) {
        const char* q = nullptr;
        if (!Take(2, &q)) return false;
        *v = ReadLeU16(q);
        return true;
    }
    bool ReadU32(uint32_t* v) {
        const char* q = nullptr;
        if (!Take(4, &q)) return false;
        *v = ReadLeU32(q);
        return true;
    }
    bool ReadI64(int64_t* v) {
        const char* q = nullptr;
        if (!Take(8, &q)) return false;
        *v = ReadLeI64(q);
        return true;
    }
    bool Skip(size_t n) {
        const char* q = nullptr;
        return Take(n, &q);
    }
};

// ---------------------------------------------------------------------------
// Key bytes. Ids use the same big-endian order as KeyPacker, and the bucket
// start additionally has its sign bit flipped so that byte order matches
// numeric order for negative (pre-1970) timestamps as well.
// ---------------------------------------------------------------------------

template <int N>
void AppendBeId(std::string* out, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = N - 1; i >= 0; --i) {
        out->push_back(static_cast<char>((u >> (8 * i)) & 0xffu));
    }
}

void AppendBeTimestamp(std::string* out, int64_t ts) {
    AppendBeId<8>(out, static_cast<int64_t>(static_cast<uint64_t>(ts) ^ (1ULL << 63)));
}

int64_t ReadBeTimestamp(const char* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u = (u << 8) | static_cast<uint8_t>(p[i]);
    }
    return static_cast<int64_t>(u ^ (1ULL << 63));
}

Point MakePoint(const Bucket& bucket, size_t p) {
    Point pt;
    pt.ts = bucket.timestamps[p];
    pt.values.resize(bucket.columns.size());
    for (size_t c = 0; c < bucket.columns.size(); ++c) pt.values[c] = bucket.columns[c][p];
    return pt;
}

// Splits a bucket at `mid`; both halves keep the column layout of the source.
void SplitBucket(const Bucket& src, size_t mid, Bucket* left, Bucket* right) {
    left->first_ts = src.timestamps.front();
    right->first_ts = src.timestamps[mid];
    left->measure_ids = src.measure_ids;
    right->measure_ids = src.measure_ids;
    left->timestamps.assign(src.timestamps.begin(), src.timestamps.begin() + mid);
    right->timestamps.assign(src.timestamps.begin() + mid, src.timestamps.end());
    left->columns.assign(src.columns.size(), std::vector<MeasureValue>());
    right->columns.assign(src.columns.size(), std::vector<MeasureValue>());
    for (size_t c = 0; c < src.columns.size(); ++c) {
        left->columns[c].assign(src.columns[c].begin(), src.columns[c].begin() + mid);
        right->columns[c].assign(src.columns[c].begin() + mid, src.columns[c].end());
    }
}

}  // namespace

bool ToMeasureType(FieldType t, MeasureType* out) {
    if (t == FieldType::DOUBLE) {
        *out = MeasureType::DOUBLE;
        return true;
    }
    if (t == FieldType::INT64) {
        *out = MeasureType::INT64;
        return true;
    }
    return false;
}

const char* MeasureTypeName(MeasureType t) {
    switch (t) {
    case MeasureType::DOUBLE:
        return "DOUBLE";
    case MeasureType::INT64:
        return "INT64";
    }
    return "UNKNOWN";
}

std::string ElementKey::ToString() const {
    const std::string kind = is_vertex ? "vertex" : "edge";
    std::string s = kind + "(" + std::to_string(uid.src);
    if (!is_vertex) {
        s += ",lid=" + std::to_string(uid.lid) + ",tid=" + std::to_string(uid.tid) +
             ",dst=" + std::to_string(uid.dst) + ",eid=" + std::to_string(uid.eid);
    }
    return s + ")";
}

const char* SeriesStore::TableName() { return _detail::SERIES_TABLE; }

std::unique_ptr<KvTable> SeriesStore::OpenTable(KvTransaction& txn, KvStore& store,
                                                const std::string& name) {
    // BYTE_SEQ (the default comparator): opaque byte keys ordered by memcmp,
    // which is what makes the prefix layout work and what keeps a time range to
    // a lower-bound seek plus a forward scan.
    return store.OpenTable(txn, name, true, ComparatorDesc::DefaultComparator());
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void SeriesStore::MakeElementPrefix(const ElementKey& elem, std::string* out) {
    out->clear();
    out->reserve(kEdgeElementPrefixSize);
    if (elem.is_vertex) {
        out->push_back(static_cast<char>(kKeyKindVertex));
        AppendBeId<5>(out, elem.uid.src);
    } else {
        out->push_back(static_cast<char>(kKeyKindEdge));
        AppendBeId<5>(out, elem.uid.src);
        AppendBeId<2>(out, elem.uid.lid);
        AppendBeId<8>(out, elem.uid.tid);
        AppendBeId<5>(out, elem.uid.dst);
        AppendBeId<4>(out, elem.uid.eid);
    }
}

void SeriesStore::MakeFieldPrefix(const ElementKey& elem, uint16_t field_id, std::string* out) {
    MakeElementPrefix(elem, out);
    AppendBeId<2>(out, field_id);
}

void SeriesStore::MakeBucketKey(const ElementKey& elem, uint16_t field_id, int64_t bucket_start,
                                std::string* out) {
    MakeFieldPrefix(elem, field_id, out);
    AppendBeTimestamp(out, bucket_start);
}

bool SeriesStore::ParseBucketKey(const Value& key, uint16_t* field_id, int64_t* bucket_start) {
    const std::string s = key.AsString();
    size_t field_off = 0;
    if (s.size() == kVertexElementPrefixSize + kFieldIdSize + kBucketStartSize) {
        field_off = kVertexElementPrefixSize;
    } else if (s.size() == kEdgeElementPrefixSize + kFieldIdSize + kBucketStartSize) {
        field_off = kEdgeElementPrefixSize;
    } else {
        return false;
    }
    *field_id = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(s[field_off])) << 8) |
        static_cast<uint8_t>(s[field_off + 1]));
    *bucket_start = ReadBeTimestamp(s.data() + field_off + kFieldIdSize);
    return true;
}

// ---------------------------------------------------------------------------
// Bucket value codec
// ---------------------------------------------------------------------------

bool SeriesStore::EncodeBucket(const Bucket& bucket, const std::vector<MeasureColumn>& columns,
                               const BucketPolicy& policy, std::string* out) {
    const size_t n = bucket.timestamps.size();
    if (n == 0) return false;
    if (policy.max_points > 0 && n > policy.max_points) return false;
    if (bucket.columns.size() != columns.size()) return false;
    if (bucket.measure_ids.size() != columns.size()) return false;
    for (const auto& col : bucket.columns) {
        if (col.size() != n) return false;
    }
    // The bucket key is the timestamp of the first point, so the two must agree.
    if (bucket.timestamps[0] != bucket.first_ts) return false;
    for (size_t i = 1; i < n; ++i) {
        if (bucket.timestamps[i] <= bucket.timestamps[i - 1]) return false;
    }

    std::string ts_column;
    if (!EncodeTimestampColumn(bucket.timestamps.data(), n, &ts_column)) return false;

    bool has_nulls = false;
    for (const auto& col : bucket.columns) {
        for (const auto& v : col) {
            if (v.is_null) {
                has_nulls = true;
                break;
            }
        }
        if (has_nulls) break;
    }

    out->clear();
    AppendLeU32(out, kBucketMagic);
    AppendLeU16(out, kBucketFormatVersion);
    AppendLeU16(out, has_nulls ? kFlagHasNulls : 0);
    AppendLeU32(out, static_cast<uint32_t>(n));
    AppendLeU16(out, static_cast<uint16_t>(columns.size()));
    for (size_t c = 0; c < columns.size(); ++c) AppendLeU16(out, bucket.measure_ids[c]);
    AppendLeI64(out, bucket.first_ts);

    AppendLeU32(out, static_cast<uint32_t>(ts_column.size()));
    out->append(ts_column);

    std::vector<double> dvals(n, 0);
    std::vector<int64_t> ivals(n, 0);
    std::vector<uint8_t> nulls(n, 0);
    std::string bitmap;
    for (size_t c = 0; c < columns.size(); ++c) {
        for (size_t p = 0; p < n; ++p) {
            const MeasureValue& v = bucket.columns[c][p];
            nulls[p] = v.is_null ? 1 : 0;
            if (columns[c].type == MeasureType::DOUBLE) {
                dvals[p] = v.d;
            } else {
                ivals[p] = v.i;
            }
        }
        EncodeNullBitmap(nulls.data(), n, &bitmap);
        std::string payload;
        ColumnEncoding encoding = ColumnEncoding::RAW64;
        bool ok = columns[c].type == MeasureType::DOUBLE
                      ? EncodeDoubleColumn(dvals.data(), nulls.data(), n, &encoding, &payload)
                      : EncodeInt64Column(ivals.data(), nulls.data(), n, &encoding, &payload);
        if (!ok) return false;
        const size_t block = 1 + bitmap.size() + payload.size();
        if (block > std::numeric_limits<uint32_t>::max()) return false;
        AppendLeU32(out, static_cast<uint32_t>(block));
        out->push_back(static_cast<char>(encoding));
        out->append(bitmap);
        out->append(payload);
    }
    return policy.max_bytes == 0 || out->size() <= policy.max_bytes;
}

bool SeriesStore::DecodeBucket(const char* data, size_t len,
                               const std::vector<MeasureColumn>& columns, Bucket* out) {
    ByteReader r(data, len);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t count = 0;
    uint16_t n_measures = 0;
    if (!r.ReadU32(&magic) || magic != kBucketMagic) return false;
    if (!r.ReadU16(&version) || version != kBucketFormatVersion) return false;
    if (!r.ReadU16(&flags) || (flags & ~kFlagHasNulls) != 0) return false;
    if (!r.ReadU32(&count) || count == 0) return false;
    if (!r.ReadU16(&n_measures) || n_measures == 0) return false;
    if (n_measures > columns.size()) return false;
    out->measure_ids.resize(n_measures);
    uint16_t prev_id = 0;
    for (uint16_t i = 0; i < n_measures; ++i) {
        uint16_t id = 0;
        if (!r.ReadU16(&id)) return false;
        if (id >= columns.size()) return false;
        if (i > 0 && id <= prev_id) return false;  // ids ascend and do not repeat
        prev_id = id;
        out->measure_ids[i] = id;
    }
    if (!r.ReadI64(&out->first_ts)) return false;

    uint32_t ts_len = 0;
    const char* ts_data = nullptr;
    if (!r.ReadU32(&ts_len)) return false;
    if (!r.Take(ts_len, &ts_data)) return false;
    out->timestamps.assign(count, 0);
    if (!DecodeTimestampColumn(ts_data, ts_len, out->first_ts, count, out->timestamps.data())) {
        return false;
    }

    const size_t bitmap_bytes = NullBitmapBytes(count);
    out->columns.assign(n_measures, std::vector<MeasureValue>());
    std::vector<uint8_t> nulls(count, 0);
    for (uint16_t c = 0; c < n_measures; ++c) {
        uint32_t block_len = 0;
        const char* block = nullptr;
        if (!r.ReadU32(&block_len)) return false;
        if (!r.Take(block_len, &block)) return false;
        if (block_len < 1 + bitmap_bytes) return false;
        const uint8_t enc = static_cast<uint8_t>(block[0]);
        if (!DecodeNullBitmap(block + 1, block_len - 1, count, nulls.data())) return false;
        const char* payload = block + 1 + bitmap_bytes;
        const size_t payload_len = block_len - 1 - bitmap_bytes;
        const MeasureType type = columns[out->measure_ids[c]].type;

        std::vector<MeasureValue> values(count, MeasureValue::Null());
        if (type == MeasureType::DOUBLE) {
            std::vector<double> tmp(count, 0);
            if (!DecodeDoubleColumn(payload, payload_len, static_cast<ColumnEncoding>(enc),
                                    nulls.data(), count, tmp.data())) {
                return false;
            }
            for (size_t p = 0; p < count; ++p) {
                if (nulls[p] == 0) values[p] = MeasureValue::Double(tmp[p]);
            }
        } else {
            std::vector<int64_t> tmp(count, 0);
            if (!DecodeInt64Column(payload, payload_len, static_cast<ColumnEncoding>(enc),
                                   nulls.data(), count, tmp.data())) {
                return false;
            }
            for (size_t p = 0; p < count; ++p) {
                if (nulls[p] == 0) values[p] = MeasureValue::Int64(tmp[p]);
            }
        }
        out->columns[c] = std::move(values);
    }
    return r.Remaining() == 0;
}

bool SeriesStore::DecodeBucketTimestamps(const char* data, size_t len, int64_t* first_ts,
                                         std::vector<int64_t>* timestamps) {
    ByteReader r(data, len);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t count = 0;
    uint16_t n_measures = 0;
    if (!r.ReadU32(&magic) || magic != kBucketMagic) return false;
    if (!r.ReadU16(&version) || version != kBucketFormatVersion) return false;
    if (!r.ReadU16(&flags) || (flags & ~kFlagHasNulls) != 0) return false;
    if (!r.ReadU32(&count) || count == 0) return false;
    if (!r.ReadU16(&n_measures) || n_measures == 0) return false;
    if (!r.Skip(static_cast<size_t>(n_measures) * 2)) return false;
    if (!r.ReadI64(first_ts)) return false;
    uint32_t ts_len = 0;
    const char* ts_data = nullptr;
    if (!r.ReadU32(&ts_len)) return false;
    if (!r.Take(ts_len, &ts_data)) return false;
    timestamps->assign(count, 0);
    return DecodeTimestampColumn(ts_data, ts_len, *first_ts, count, timestamps->data());
}

// ---------------------------------------------------------------------------
// Bucket lookup and writes
// ---------------------------------------------------------------------------

bool SeriesStore::FindBucketFloor(KvTransaction& txn, const std::string& field_prefix, int64_t ts,
                                 std::string* key, bool* found) const {
    *found = false;
    std::string target = field_prefix;
    AppendBeTimestamp(&target, ts);
    const Value target_val = Value::ConstRef(target);
    auto it = table_->GetClosestIterator(txn, target_val);
    if (!it->IsValid()) {
        // Nothing at or after the target: the last key in the table is the
        // only possible predecessor.
        if (!it->GotoLastKey()) return true;  // table is empty
    } else if (table_->CompareKey(txn, it->GetKey(), target_val) != 0) {
        // The closest key lies past the target, so the bucket we want is its
        // predecessor. One step back is enough: any key strictly between two
        // keys that share a prefix also shares that prefix, so there cannot be
        // a nearer candidate.
        auto first = table_->GetIterator(txn);
        if (!first->GotoFirstKey()) return true;
        if (table_->CompareKey(txn, first->GetKey(), it->GetKey()) == 0) {
            return true;  // it is the first key in the table: no predecessor
        }
        it->Prev();
    }
    if (!it->IsValid()) return true;
    const std::string candidate = it->GetKey().AsString();
    if (!fma_common::StartsWith(candidate, field_prefix)) return true;
    *key = candidate;
    *found = true;
    return true;
}

bool SeriesStore::FindScanStart(KvTransaction& txn, const std::string& field_prefix, int64_t t0,
                                std::string* key, bool* found) const {
    if (!FindBucketFloor(txn, field_prefix, t0, key, found)) return false;
    if (*found) return true;
    // Every bucket of this field starts after t0, so the field's first bucket
    // is where the scan has to begin: its points are all >= its first_ts, and
    // the range filter drops whatever falls before t0.
    auto it = table_->GetClosestIterator(txn, Value::ConstRef(field_prefix));
    if (!it->IsValid()) return true;
    const std::string candidate = it->GetKey().AsString();
    if (!fma_common::StartsWith(candidate, field_prefix)) return true;
    *key = candidate;
    *found = true;
    return true;
}

bool SeriesStore::WriteBucket(KvTransaction& txn, const std::string& field_prefix,
                             const Bucket& bucket, const std::vector<MeasureColumn>& columns,
                             const BucketPolicy& policy) const {
    std::string key = field_prefix;
    AppendBeTimestamp(&key, bucket.first_ts);

    const bool within_point_cap =
        policy.max_points == 0 || bucket.PointCount() <= policy.max_points;
    if (within_point_cap) {
        std::string encoded;
        if (EncodeBucket(bucket, columns, policy, &encoded)) {
            table_->SetValue(txn, Value::ConstRef(key), Value::ConstRef(encoded), true);
            return true;
        }
    }
    // Either the point cap or the byte cap was reached. Split in half and store
    // both halves; the left half keeps this key because it still starts at the
    // same timestamp. A failure here may have stored the left half already, so
    // the caller has to treat false as fatal for the transaction.
    if (bucket.PointCount() < 2) return false;
    const size_t mid = bucket.PointCount() / 2;
    Bucket left;
    Bucket right;
    SplitBucket(bucket, mid, &left, &right);
    return WriteBucket(txn, field_prefix, left, columns, policy) &&
           WriteBucket(txn, field_prefix, right, columns, policy);
}

void SeriesStore::DeleteByPrefix(KvTransaction& txn, const std::string& prefix) const {
    const Value prefix_val = Value::ConstRef(prefix);
    for (auto it = table_->GetClosestIterator(txn, prefix_val); it->IsValid();) {
        // GetKey() refers to the key being deleted, so decide before deleting.
        if (!fma_common::StartsWith(it->GetKey().AsString(), prefix)) break;
        it->DeleteKey();
    }
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

bool SeriesStore::Upsert(KvTransaction& txn, const ElementKey& elem, uint16_t field_id,
                         int64_t ts, const std::vector<MeasureValue>& values,
                         const std::vector<MeasureColumn>& columns, const BucketPolicy& policy) {
    const size_t n_measures = columns.size();
    if (n_measures == 0 || values.size() != n_measures) return false;

    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);

    std::string key;
    bool found = false;
    if (!FindBucketFloor(txn, field_prefix, ts, &key, &found)) return false;

    Bucket bucket;
    if (found) {
        const Value stored = table_->GetValue(txn, Value::ConstRef(key), true);
        // Fails if the stored bucket's measures do not match `columns`, e.g.
        // after the label's series measures were redefined.
        if (!DecodeBucket(stored.Data(), stored.Size(), columns, &bucket)) return false;
        if (policy.max_span_us > 0 && ts > bucket.LastTs() &&
            static_cast<uint64_t>(ts - bucket.first_ts) > policy.max_span_us) {
            // Stretching this bucket would break the span cap: start a new one.
            // No bucket can exist at exactly ts, or the lookup would have
            // returned it.
            found = false;
        }
    }

    if (!found) {
        bucket = Bucket();
        bucket.first_ts = ts;
        bucket.timestamps.push_back(ts);
        bucket.measure_ids.resize(n_measures);
        bucket.columns.assign(n_measures, std::vector<MeasureValue>(1));
        for (size_t c = 0; c < n_measures; ++c) {
            bucket.measure_ids[c] = static_cast<uint16_t>(c);
            bucket.columns[c][0] = values[c];
        }
        return WriteBucket(txn, field_prefix, bucket, columns, policy);
    }

    const auto pos = std::lower_bound(bucket.timestamps.begin(), bucket.timestamps.end(), ts);
    const size_t p = static_cast<size_t>(pos - bucket.timestamps.begin());
    if (pos != bucket.timestamps.end() && *pos == ts) {
        // Correction of an existing point: overwrite its values in place.
        for (size_t c = 0; c < n_measures; ++c) bucket.columns[c][p] = values[c];
    } else {
        bucket.timestamps.insert(pos, ts);
        for (size_t c = 0; c < n_measures; ++c) {
            bucket.columns[c].insert(bucket.columns[c].begin() + p, values[c]);
        }
    }
    return WriteBucket(txn, field_prefix, bucket, columns, policy);
}

bool SeriesStore::Range(KvTransaction& txn, const ElementKey& elem, uint16_t field_id, int64_t t0,
                        int64_t t1, const std::vector<MeasureColumn>& columns,
                        std::vector<Point>* points) const {
    points->clear();
    if (t0 > t1 || columns.empty()) return true;

    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    std::string key;
    bool found = false;
    if (!FindScanStart(txn, field_prefix, t0, &key, &found)) return false;
    if (!found) return true;

    const Value key_val = Value::ConstRef(key);
    for (auto it = table_->GetClosestIterator(txn, key_val); it->IsValid(); it->Next()) {
        const std::string bucket_key = it->GetKey().AsString();
        if (!fma_common::StartsWith(bucket_key, field_prefix)) break;
        const Value stored = it->GetValue();
        Bucket bucket;
        if (!DecodeBucket(stored.Data(), stored.Size(), columns, &bucket)) return false;
        if (bucket.first_ts > t1) break;
        for (size_t p = 0; p < bucket.PointCount(); ++p) {
            const int64_t ts = bucket.timestamps[p];
            if (ts < t0) continue;
            if (ts > t1) break;
            points->push_back(MakePoint(bucket, p));
        }
    }
    return true;
}

bool SeriesStore::Count(KvTransaction& txn, const ElementKey& elem, uint16_t field_id, int64_t t0,
                        int64_t t1, const std::vector<MeasureColumn>& columns,
                        size_t* count) const {
    *count = 0;
    if (t0 > t1 || columns.empty()) return true;

    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    std::string key;
    bool found = false;
    if (!FindScanStart(txn, field_prefix, t0, &key, &found)) return false;
    if (!found) return true;

    std::vector<int64_t> timestamps;
    const Value key_val = Value::ConstRef(key);
    for (auto it = table_->GetClosestIterator(txn, key_val); it->IsValid(); it->Next()) {
        const std::string bucket_key = it->GetKey().AsString();
        if (!fma_common::StartsWith(bucket_key, field_prefix)) break;
        const Value stored = it->GetValue();
        int64_t first_ts = 0;
        if (!DecodeBucketTimestamps(stored.Data(), stored.Size(), &first_ts, &timestamps)) {
            return false;
        }
        if (first_ts > t1) break;
        if (first_ts >= t0 && timestamps.back() <= t1) {
            // The bucket lies entirely inside the range: the header count is
            // all we needed.
            *count += timestamps.size();
            continue;
        }
        for (int64_t ts : timestamps) {
            if (ts < t0) continue;
            if (ts > t1) break;
            ++(*count);
        }
    }
    return true;
}

bool SeriesStore::Latest(KvTransaction& txn, const ElementKey& elem, uint16_t field_id,
                         const std::vector<MeasureColumn>& columns, Point* point) const {
    *point = Point();
    if (columns.empty()) return true;

    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    std::string key;
    bool found = false;
    if (!FindBucketFloor(txn, field_prefix, kMaxTs, &key, &found)) return false;
    if (!found) return true;

    const Value stored = table_->GetValue(txn, Value::ConstRef(key));
    Bucket bucket;
    if (!DecodeBucket(stored.Data(), stored.Size(), columns, &bucket)) return false;
    if (bucket.PointCount() == 0) return true;
    *point = MakePoint(bucket, bucket.PointCount() - 1);
    return true;
}

bool SeriesStore::Earliest(KvTransaction& txn, const ElementKey& elem, uint16_t field_id,
                           const std::vector<MeasureColumn>& columns, Point* point) const {
    *point = Point();
    if (columns.empty()) return true;

    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    std::string key;
    bool found = false;
    if (!FindScanStart(txn, field_prefix, kMinTs, &key, &found)) return false;
    if (!found) return true;

    const Value stored = table_->GetValue(txn, Value::ConstRef(key));
    Bucket bucket;
    if (!DecodeBucket(stored.Data(), stored.Size(), columns, &bucket)) return false;
    if (bucket.PointCount() == 0) return true;
    *point = MakePoint(bucket, 0);
    return true;
}

bool SeriesStore::HasSeries(KvTransaction& txn, const ElementKey& elem, uint16_t field_id) const {
    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    std::string key;
    bool found = false;
    if (!FindBucketFloor(txn, field_prefix, kMaxTs, &key, &found)) return false;
    return found;
}

void SeriesStore::DeleteField(KvTransaction& txn, const ElementKey& elem, uint16_t field_id) const {
    std::string field_prefix;
    MakeFieldPrefix(elem, field_id, &field_prefix);
    DeleteByPrefix(txn, field_prefix);
}

void SeriesStore::DeleteElement(KvTransaction& txn, const ElementKey& elem) const {
    std::string elem_prefix;
    MakeElementPrefix(elem, &elem_prefix);
    DeleteByPrefix(txn, elem_prefix);
}

}  // namespace series
}  // namespace lgraph
