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
#include <limits>
#include <string>
#include <vector>

#include "lgraph/lgraph_types.h"

namespace lgraph {
namespace series {

/**
 * A measure column is a DOUBLE or an INT64. Series DDL rejects every other
 * type, so the store never has to deal with one; `ToMeasureType` is the single
 * place where a FieldType is turned into a column type.
 */
enum class MeasureType : uint8_t { DOUBLE = 0, INT64 = 1 };

/** The single place a FieldType is turned into a measure type; anything else is
 *  rejected at DDL time, so the store only ever sees these two. */
inline bool ToMeasureType(FieldType t, MeasureType* out) {
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

inline const char* MeasureTypeName(MeasureType t) {
    return t == MeasureType::DOUBLE ? "DOUBLE" : "INT64";
}

struct MeasureColumn {
    MeasureType type = MeasureType::DOUBLE;
};

/**
 * Bucket closing policy, mirroring the knobs on the schema's series spec.
 *
 * max_points  - close a bucket when it grows past this many points.
 * max_span_us - optional wall-clock cap; 0 disables it. With daily bars a
 *               wall-clock cap would produce one-point buckets, so it is off by
 *               default and the point cap alone bounds the rewrite cost.
 * max_bytes   - hard cap on an encoded bucket. This bounds the copy-on-write
 *               rewrite cost of an in-place correction, and in the store it is
 *               what forces a split when a pathological bucket does not fit the
 *               point cap. Note that the KV layer does not enforce anything
 *               itself: LMDBKvTable will happily store a record of any size, so
 *               the check has to live here (max_points also keeps us far under
 *               MAX_PROP_SIZE, and the bucket key is far under MAX_KEY_SIZE).
 */
struct BucketPolicy {
    uint32_t max_points = 1000;
    uint64_t max_span_us = 0;
    uint32_t max_bytes = 1u << 20;
};

/** Unbounded endpoints for the read API. */
static const int64_t kMinTs = std::numeric_limits<int64_t>::min();
static const int64_t kMaxTs = std::numeric_limits<int64_t>::max();

/** One measure value at one point. */
struct MeasureValue {
    bool is_null = true;
    double d = 0;
    int64_t i = 0;

    static MeasureValue Null() { return MeasureValue(); }
    static MeasureValue Double(double v) {
        MeasureValue m;
        m.is_null = false;
        m.d = v;
        return m;
    }
    static MeasureValue Int64(int64_t v) {
        MeasureValue m;
        m.is_null = false;
        m.i = v;
        return m;
    }
};

/**
 * One point as handed back to callers: its timestamp and one value per column
 * in the order the caller asked for.
 */
struct Point {
    int64_t ts = 0;
    std::vector<MeasureValue> values;
};

/**
 * A decoded bucket value. `columns[c][p]` is measure column c at point p, and
 * `measure_ids[c]` is the schema measure index that column came from. Decoding
 * is generic over the ids present, but the store always writes every measure of
 * the field in schema order, so ids are 0..n-1 today.
 */
struct Bucket {
    int64_t first_ts = 0;
    std::vector<uint16_t> measure_ids;
    std::vector<int64_t> timestamps;
    std::vector<std::vector<MeasureValue>> columns;

    size_t PointCount() const { return timestamps.size(); }
    int64_t LastTs() const { return timestamps.empty() ? first_ts : timestamps.back(); }
};

/** A measure as the read API needs it: its name and how to read a value. */
struct MeasureRef {
    std::string name;
    MeasureType type = MeasureType::DOUBLE;
};

/**
 * Header-only state of one series field: how many points it holds, where it
 * starts and ends, and what its measures are called.
 *
 * Everything here comes from a bucket header plus one bucket end, so it is what
 * a summary can report without materialising the series - and it is also how a
 * caller learns a field's measure names, which the encoded buckets do not carry
 * (they store measure indices, not names).
 */
struct SeriesSummary {
    size_t count = 0;
    bool has_points = false;
    int64_t first_ts = 0;  // microseconds; valid only if has_points
    int64_t last_ts = 0;   // microseconds; valid only if has_points
    std::vector<MeasureRef> measures;
};

/**
 * The identity of the element a series hangs off, as it appears in the KV key.
 * A vertex is addressed by its vid; an edge by the fields KeyPacker puts in an
 * edge key (src, lid, tid, dst, eid), in that byte order.
 */
struct ElementKey {
    bool is_vertex = true;
    lgraph_api::EdgeUid uid;  // for a vertex, only uid.src (the vid) is meaningful

    static ElementKey FromVertex(int64_t vid) {
        ElementKey k;
        k.is_vertex = true;
        k.uid.src = vid;
        return k;
    }
    static ElementKey FromEdge(const lgraph_api::EdgeUid& uid) {
        ElementKey k;
        k.is_vertex = false;
        k.uid = uid;
        return k;
    }
    /** Human-readable form for error messages. */
    std::string ToString() const;
};

}  // namespace series
}  // namespace lgraph
