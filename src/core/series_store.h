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

#include <memory>
#include <string>
#include <vector>

#include "core/kv_store.h"
#include "core/series_types.h"

namespace lgraph {
namespace series {

/**
 * Time-series storage: one KV table per graph (`_tseries_`), one key per
 * bucket, one value per bucket. Buckets are ranges of consecutive points of a
 * single (element, field) pair, so a time range is one lower-bound seek plus a
 * short forward scan, and a correction only rewrites the bucket it lands in.
 *
 * Key layout, all integers big-endian (the same byte order KeyPacker uses), so
 * BYTE_SEQ ordering is numeric ordering and all the keys of one (element,
 * field) are contiguous:
 *
 *   vertex: [0x00][vid:5][field_id:2][bucket_start:8]            = 16 B
 *   edge:   [0x01][src:5][lid:2][tid:8][dst:5][eid:4][field:2][start:8] = 35 B
 *
 * bucket_start is the timestamp of the bucket's first point, stored with the
 * sign bit flipped. Plain big-endian two's complement sorts every negative
 * value *after* every positive one, which would break both range scans and
 * bucket lookup for pre-1970 timestamps; the flip makes byte order agree with
 * numeric order over the whole int64 range. The plan's plain big-endian is only
 * correct for non-negative timestamps.
 *
 * Value layout (little-endian, unlike the keys):
 *
 *   magic        u32   "TSB1"
 *   version      u16   1
 *   flags        u16   bit0 = any nulls present
 *   count        u32   points in this bucket (0 < count <= policy.max_points)
 *   n_measures   u16
 *   measure_ids  u16[n_measures]   schema index of each column present
 *   first_ts     i64   exact us timestamp of point[0]; also the key suffix
 *   then, in order:
 *     TS column:     u32 byte_len | delta-of-delta bitstream (see
 *                    series_encoding.h; it does not repeat first_ts)
 *     per measure:   u32 byte_len | u8 encoding | null bitmap | payload
 *                    byte_len covers the encoding byte, the bitmap and the
 *                    payload; the bitmap is always present, even when the
 *                    column has no nulls.
 *
 * The store owns its table, mirroring BlobManager.
 */
class SeriesStore {
 public:
    static const char* TableName();

    /** Opens (creating on demand) the series table of a graph. */
    static std::unique_ptr<KvTable> OpenTable(KvTransaction& txn, KvStore& store,
                                             const std::string& name);

    SeriesStore(KvTransaction& txn, std::unique_ptr<KvTable> table) : table_(std::move(table)) {}

    KvTable* Table() const { return table_.get(); }

    /**
     * Writes one point: appends it, or overwrites the values of the point that
     * already sits at `ts` (idempotent correction/restatement).
     *
     * A point older than every existing bucket starts a new bucket rather than
     * being merged backwards, because a bucket's key is its first timestamp.
     * Back-filling history in descending order therefore fragments into one
     * bucket per point; appending in time order does not.
     *
     * `values[i]` is the value of measure column i; `columns` describes the
     * field's measures in schema order. Returns false if the arguments are
     * inconsistent, if the stored bucket was written with a different measure
     * set than `columns` (e.g. the measures were redefined), or if a single
     * point cannot be made to fit the bucket byte cap.
     */
    bool Upsert(KvTransaction& txn, const ElementKey& elem, uint16_t field_id, int64_t ts,
                const std::vector<MeasureValue>& values,
                const std::vector<MeasureColumn>& columns, const BucketPolicy& policy);

    /**
     * Reads every point with t0 <= ts <= t1 in ascending time order. Ranges are
     * inclusive and may be unbounded via kMinTs/kMaxTs. An empty range is not an
     * error. Returns false only if a stored bucket is malformed.
     */
    bool Range(KvTransaction& txn, const ElementKey& elem, uint16_t field_id, int64_t t0,
               int64_t t1, const std::vector<MeasureColumn>& columns,
               std::vector<Point>* points) const;

    /** Number of points in [t0, t1]. Reads the header, and the timestamp column
     *  only where the range cuts a bucket, so it stays cheap; a consequence is
     *  that corruption confined to a measure column is not visible to it. */
    bool Count(KvTransaction& txn, const ElementKey& elem, uint16_t field_id, int64_t t0,
               int64_t t1, const std::vector<MeasureColumn>& columns, size_t* count) const;

    /**
     * The newest / oldest point of the field, read from one end of one bucket.
     * `point` is left with no values when the field has no points at all;
     * returns false only for a malformed stored bucket.
     */
    bool Latest(KvTransaction& txn, const ElementKey& elem, uint16_t field_id,
                const std::vector<MeasureColumn>& columns, Point* point) const;
    bool Earliest(KvTransaction& txn, const ElementKey& elem, uint16_t field_id,
                  const std::vector<MeasureColumn>& columns, Point* point) const;

    /** True if any bucket exists for this (element, field). */
    bool HasSeries(KvTransaction& txn, const ElementKey& elem, uint16_t field_id) const;

    /** Drops every bucket of one field, or of the whole element (used when a
     *  vertex/edge is deleted and when a series field is dropped from a label). */
    void DeleteField(KvTransaction& txn, const ElementKey& elem, uint16_t field_id) const;
    void DeleteElement(KvTransaction& txn, const ElementKey& elem) const;

    // --- key and value codecs, exposed for tests and for the import path ---

    /** The element's key prefix, i.e. its key with no field id and no bucket. */
    static void MakeElementPrefix(const ElementKey& elem, std::string* out);
    /** The element's key prefix up to and including field_id. */
    static void MakeFieldPrefix(const ElementKey& elem, uint16_t field_id, std::string* out);
    static void MakeBucketKey(const ElementKey& elem, uint16_t field_id, int64_t bucket_start,
                              std::string* out);
    /** Parses the trailing field id and bucket start of a bucket key. */
    static bool ParseBucketKey(const Value& key, uint16_t* field_id, int64_t* bucket_start);

    static bool EncodeBucket(const Bucket& bucket, const std::vector<MeasureColumn>& columns,
                             const BucketPolicy& policy, std::string* out);
    static bool DecodeBucket(const char* data, size_t len,
                             const std::vector<MeasureColumn>& columns, Bucket* out);
    /** Decodes the header and the timestamp column only. */
    static bool DecodeBucketTimestamps(const char* data, size_t len, int64_t* first_ts,
                                       std::vector<int64_t>* timestamps);

 private:
    /**
     * Finds the last bucket of (element, field) whose start is <= ts, i.e. the
     * bucket a point at `ts` belongs to. `key` receives the bucket key when
     * found. Used for writes, where a point older than every existing bucket
     * has to start a new one rather than join the first.
     */
    bool FindBucketFloor(KvTransaction& txn, const std::string& field_prefix, int64_t ts,
                         std::string* key, bool* found) const;

    /**
     * Like FindBucketFloor, but for a range scan: when no bucket starts at or
     * before t0, the scan has to begin at the field's first bucket instead,
     * because that bucket owns every timestamp below the next bucket's start
     * (including t0 < kMinTs-style queries such as the unbounded lower end).
     */
    bool FindScanStart(KvTransaction& txn, const std::string& field_prefix, int64_t t0,
                       std::string* key, bool* found) const;

    /** Encodes and stores a bucket, splitting it in half if it does not fit. */
    bool WriteBucket(KvTransaction& txn, const std::string& field_prefix, const Bucket& bucket,
                     const std::vector<MeasureColumn>& columns, const BucketPolicy& policy) const;

    /** Stores every bucket of the element whose field prefix matches. */
    void DeleteByPrefix(KvTransaction& txn, const std::string& prefix) const;

    std::unique_ptr<KvTable> table_;
};

}  // namespace series
}  // namespace lgraph
