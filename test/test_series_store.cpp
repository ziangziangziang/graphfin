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

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "./ut_utils.h"
#include "fma-common/file_system.h"
#include "fma-common/string_formatter.h"

#include "core/lmdb_store.h"
#include "core/series_store.h"

namespace {

using namespace lgraph;          // NOLINT
using namespace lgraph::series;  // NOLINT

const int64_t kUsPerDay = 86400LL * 1000000LL;

const BucketPolicy kDefaultPolicy;

// Four DOUBLE measures plus one INT64, matching Point5()'s layout.
std::vector<MeasureColumn> OhlcColumns() {
    std::vector<MeasureColumn> cols(5, MeasureColumn{MeasureType::DOUBLE});
    cols[4].type = MeasureType::INT64;
    return cols;
}

MeasureValue D(double v) { return MeasureValue::Double(v); }
MeasureValue I(int64_t v) { return MeasureValue::Int64(v); }
MeasureValue N() { return MeasureValue::Null(); }

/**
 * One LMDB graph-style store, its write transaction and a series table. Each
 * test works in its own directory; Reopen() destroys everything and reads the
 * same directory back, which is what the graph does when it closes and reopens.
 */
class SeriesSession {
 public:
    explicit SeriesSession(const std::string& dir) { Open(dir); }

    void Open(const std::string& dir) {
        store_.reset(new LMDBKvStore(dir));
        txn_ = store_->CreateWriteTxn();
        auto table = SeriesStore::OpenTable(*txn_, *store_, SeriesStore::TableName());
        // Commit the table creation before handing out a transaction to the
        // test, exactly as LightningGraph::Open does: a table opened inside a
        // transaction that is later aborted is not usable.
        txn_->Commit();
        txn_ = store_->CreateWriteTxn();
        series_.reset(new SeriesStore(*txn_, std::move(table)));
    }

    void Close() {
        series_.reset();
        txn_.reset();
        store_.reset();
    }

    void Commit() {
        txn_->Commit();
        txn_ = store_->CreateWriteTxn();
    }

    void Abort() {
        txn_->Abort();
        txn_ = store_->CreateWriteTxn();
    }

    KvTransaction& txn() { return *txn_; }
    SeriesStore& series() { return *series_; }

 private:
    std::unique_ptr<LMDBKvStore> store_;
    std::unique_ptr<KvTransaction> txn_;
    std::unique_ptr<SeriesStore> series_;
};

std::vector<MeasureValue> Point5(double base) {
    return {D(base), D(base + 1), D(base + 0.5), D(base + 1.5), I(static_cast<int64_t>(base))};
}

std::vector<int64_t> RangeTimes(const std::vector<Point>& points) {
    std::vector<int64_t> ts;
    for (const Point& p : points) ts.push_back(p.ts);
    return ts;
}

}  // namespace

class TestSeriesStore : public TuGraphTest {
 protected:
    void SetUp() override {
        dir_ = "./test_series_store_db";
        fma_common::file_system::RemoveDir(dir_);
        fma_common::file_system::MkDir(dir_);
    }
    void TearDown() override { fma_common::file_system::RemoveDir(dir_); }
    std::string dir_;
};

TEST_F(TestSeriesStore, KeyLayoutAndOrdering) {
    const ElementKey vertex = ElementKey::FromVertex(7);
    std::string elem;
    SeriesStore::MakeElementPrefix(vertex, &elem);
    EXPECT_EQ(elem.size(), 6u);

    std::string key;
    SeriesStore::MakeBucketKey(vertex, 3, 1000, &key);
    EXPECT_EQ(key.size(), 16u);
    EXPECT_EQ(static_cast<uint8_t>(key[0]), 0u);  // vertex kind
    uint16_t field_id = 0;
    int64_t bucket_start = 0;
    EXPECT_TRUE(SeriesStore::ParseBucketKey(Value::ConstRef(key), &field_id, &bucket_start));
    EXPECT_EQ(field_id, 3);
    EXPECT_EQ(bucket_start, 1000);

    const ElementKey edge = ElementKey::FromEdge(lgraph_api::EdgeUid(1, 2, 3, 4, 5));
    SeriesStore::MakeBucketKey(edge, 9, -5, &key);
    EXPECT_EQ(key.size(), 35u);
    EXPECT_EQ(static_cast<uint8_t>(key[0]), 1u);  // edge kind
    EXPECT_TRUE(SeriesStore::ParseBucketKey(Value::ConstRef(key), &field_id, &bucket_start));
    EXPECT_EQ(field_id, 9);
    EXPECT_EQ(bucket_start, -5);

    // A malformed or truncated key is rejected rather than parsed as garbage.
    const std::string short_key(15, '\0');
    EXPECT_FALSE(SeriesStore::ParseBucketKey(Value::ConstRef(short_key), &field_id, &bucket_start));

    // Buckets sort by timestamp across the whole int64 range, including
    // negative (pre-1970) timestamps, so a range scan works.
    std::string before_epoch;
    std::string after_epoch;
    SeriesStore::MakeBucketKey(vertex, 0, -1, &before_epoch);
    SeriesStore::MakeBucketKey(vertex, 0, 1, &after_epoch);
    EXPECT_LT(before_epoch, after_epoch);
    std::string earlier;
    SeriesStore::MakeBucketKey(vertex, 0, -2, &earlier);
    EXPECT_LT(earlier, before_epoch);
}

TEST_F(TestSeriesStore, AppendsCorrectsAndReads) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(1);
    const std::vector<MeasureColumn> cols = OhlcColumns();

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay,
                                            Point5(100.0 + i), cols, kDefaultPolicy));
    }
    session.Commit();

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 5u);
    EXPECT_EQ(points[0].ts, 0);
    EXPECT_EQ(points[4].ts, 4 * kUsPerDay);
    EXPECT_EQ(points[2].values.size(), 5u);
    EXPECT_DOUBLE_EQ(points[2].values[0].d, 102.0);
    EXPECT_EQ(points[2].values[4].i, 102);

    size_t count = 0;
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kMinTs, kMaxTs, cols, &count));
    EXPECT_EQ(count, 5u);

    Point latest;
    ASSERT_TRUE(session.series().Latest(session.txn(), v, 0, cols, &latest));
    EXPECT_EQ(latest.ts, 4 * kUsPerDay);
    EXPECT_DOUBLE_EQ(latest.values[0].d, 104.0);
    Point earliest;
    ASSERT_TRUE(session.series().Earliest(session.txn(), v, 0, cols, &earliest));
    EXPECT_EQ(earliest.ts, 0);
    EXPECT_DOUBLE_EQ(earliest.values[0].d, 100.0);

    // Correcting a point overwrites it instead of appending a second one, and
    // doing it twice leaves the stored bytes untouched.
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 2 * kUsPerDay, Point5(200.0), cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 2 * kUsPerDay, Point5(200.0), cols,
                                        kDefaultPolicy));
    session.Commit();

    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 5u);
    EXPECT_DOUBLE_EQ(points[2].values[0].d, 200.0);
    EXPECT_DOUBLE_EQ(points[2].values[3].d, 201.5);
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kMinTs, kMaxTs, cols, &count));
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 1u);
}

TEST_F(TestSeriesStore, OutOfOrderInsertsKeepOneBucketOrdered) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(1);
    const std::vector<MeasureColumn> cols = OhlcColumns();

    // Insert in a deliberately scrambled order within one bucket.
    const int order[] = {3, 0, 4, 1, 2};
    for (int i : order) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                            kDefaultPolicy));
    }
    session.Commit();

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_EQ(RangeTimes(points), (std::vector<int64_t>{0, kUsPerDay, 2 * kUsPerDay,
                                                       3 * kUsPerDay, 4 * kUsPerDay}));
    EXPECT_DOUBLE_EQ(points[3].values[0].d, 3.0);
    EXPECT_EQ(points[3].values[4].i, 3);
    // A point older than the earliest bucket starts a new bucket instead of
    // being merged backwards (its timestamp is the bucket key), so back-filling
    // in descending order fragments on purpose: here ts=3 opened a bucket and
    // 0,1,2 opened another.
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 2u);
}

TEST_F(TestSeriesStore, SplitsWhenPointCapIsReached) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(1);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    BucketPolicy policy;
    policy.max_points = 4;

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                            policy));
    }
    session.Commit();

    // An over-full bucket is split in half (not at the cap), so 10 points with
    // a cap of 4 end up as 4 buckets of at most 4 points each.
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 4u);

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(points[i].ts, i * kUsPerDay);
        EXPECT_DOUBLE_EQ(points[i].values[0].d, static_cast<double>(i));
        EXPECT_EQ(points[i].values[4].i, i);
    }
    // Every bucket key equals the timestamp of its first point, and no bucket
    // holds more than the cap.
    std::vector<int64_t> bucket_starts;
    auto it = session.series().Table()->GetIterator(session.txn());
    ASSERT_TRUE(it->GotoFirstKey());
    for (; it->IsValid(); it->Next()) {
        uint16_t field_id = 0;
        int64_t bucket_start = 0;
        ASSERT_TRUE(SeriesStore::ParseBucketKey(it->GetKey(), &field_id, &bucket_start));
        EXPECT_EQ(field_id, 0);
        bucket_starts.push_back(bucket_start);
    }
    ASSERT_EQ(bucket_starts.size(), 4u);
    for (size_t b = 0; b < bucket_starts.size(); ++b) {
        const int64_t last = b + 1 < bucket_starts.size() ? bucket_starts[b + 1] - 1 : kMaxTs;
        std::vector<Point> bucket_points;
        ASSERT_TRUE(session.series().Range(session.txn(), v, 0, bucket_starts[b], last, cols,
                                           &bucket_points));
        EXPECT_GE(bucket_points.size(), 1u);
        EXPECT_LE(bucket_points.size(), policy.max_points);
        ASSERT_FALSE(bucket_points.empty());
        EXPECT_EQ(bucket_points[0].ts, bucket_starts[b]);
    }

    size_t count = 0;
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kMinTs, kMaxTs, cols, &count));
    EXPECT_EQ(count, 10u);
}

TEST_F(TestSeriesStore, SplitsWhenByteCapIsReached) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(2);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    BucketPolicy policy;
    policy.max_points = 1000;
    policy.max_bytes = 128;  // a few points' worth, so the cap has to bite

    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                            policy));
    }
    session.Commit();

    EXPECT_GT(session.series().Table()->GetKeyCount(session.txn()), 1u);
    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(points[i].ts, i * kUsPerDay);
        EXPECT_DOUBLE_EQ(points[i].values[1].d, static_cast<double>(i) + 1);
    }
    // The stored buckets respect the cap that forced the splits.
    auto it = session.series().Table()->GetIterator(session.txn());
    ASSERT_TRUE(it->GotoFirstKey());
    for (; it->IsValid(); it->Next()) {
        EXPECT_LE(it->GetValue().Size(), policy.max_bytes);
    }
}

TEST_F(TestSeriesStore, SpanCapStartsNewBuckets) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(3);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    BucketPolicy policy;
    policy.max_span_us = 2 * static_cast<uint64_t>(kUsPerDay);

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                            policy));
    }
    session.Commit();

    // Buckets [0,1,2] [3,4,5] [6,7,8] [9].
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 4u);
    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_EQ(points.size(), 10u);
}

TEST_F(TestSeriesStore, RangeBoundsAreInclusiveAndClamped) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(4);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                            kDefaultPolicy));
    }
    session.Commit();

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kUsPerDay, 3 * kUsPerDay, cols,
                                       &points));
    EXPECT_EQ(RangeTimes(points), (std::vector<int64_t>{kUsPerDay, 2 * kUsPerDay,
                                                       3 * kUsPerDay}));

    // A window that lands between two points, and one that misses entirely.
    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kUsPerDay / 2, 3 * kUsPerDay, cols,
                                       &points));
    EXPECT_EQ(points.size(), 3u);
    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, 10 * kUsPerDay, 20 * kUsPerDay, cols,
                                       &points));
    EXPECT_TRUE(points.empty());
    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kUsPerDay, 0, cols, &points));
    EXPECT_TRUE(points.empty());

    size_t count = 0;
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kUsPerDay, 3 * kUsPerDay, cols,
                                       &count));
    EXPECT_EQ(count, 3u);
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kUsPerDay + 1, 3 * kUsPerDay - 1,
                                       cols, &count));
    EXPECT_EQ(count, 1u);

    // Pre-epoch data is reachable through the same range scan, and an
    // unbounded-below query starts at the field's first bucket.
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, -10 * kUsPerDay, Point5(-10), cols,
                                        kDefaultPolicy));
    session.Commit();
    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, 0, cols, &points));
    ASSERT_EQ(points.size(), 2u);  // the pre-epoch point and the one at ts 0
    EXPECT_EQ(points[0].ts, -10 * kUsPerDay);
    EXPECT_EQ(points[1].ts, 0);
    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_EQ(points.size(), 6u);
    ASSERT_TRUE(session.series().Earliest(session.txn(), v, 0, cols, &points[0]));
    EXPECT_EQ(points[0].ts, -10 * kUsPerDay);
}

TEST_F(TestSeriesStore, NullMeasuresRoundTrip) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(5);
    const std::vector<MeasureColumn> cols = OhlcColumns();

    std::vector<MeasureValue> with_null = Point5(10.0);
    with_null[2] = N();
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 0, with_null, cols, kDefaultPolicy));
    std::vector<MeasureValue> all_null(5, N());
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, kUsPerDay, all_null, cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 2 * kUsPerDay, Point5(12.0), cols,
                                        kDefaultPolicy));
    session.Commit();

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 3u);
    EXPECT_TRUE(points[0].values[2].is_null);  // the measure that was left out
    EXPECT_DOUBLE_EQ(points[0].values[0].d, 10.0);
    EXPECT_DOUBLE_EQ(points[0].values[3].d, 11.5);
    EXPECT_EQ(points[0].values[4].i, 10);
    EXPECT_TRUE(points[1].values[0].is_null);
    EXPECT_TRUE(points[1].values[4].is_null);
    EXPECT_FALSE(points[2].values[4].is_null);
    EXPECT_EQ(points[2].values[4].i, 12);
    EXPECT_DOUBLE_EQ(points[2].values[2].d, 12.5);
}

TEST_F(TestSeriesStore, ElementsAndFieldsAreIsolated) {
    SeriesSession session(dir_);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    const ElementKey v1 = ElementKey::FromVertex(1);
    const ElementKey v2 = ElementKey::FromVertex(2);
    const ElementKey e1 = ElementKey::FromEdge(lgraph_api::EdgeUid(1, 2, 3, 4, 5));

    ASSERT_TRUE(session.series().Upsert(session.txn(), v1, 0, 0, Point5(1.0), cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), v1, 1, 0, Point5(2.0), cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), v2, 0, 0, Point5(3.0), cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), e1, 0, 0, Point5(4.0), cols,
                                        kDefaultPolicy));
    session.Commit();
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 4u);

    EXPECT_TRUE(session.series().HasSeries(session.txn(), v1, 0));
    EXPECT_TRUE(session.series().HasSeries(session.txn(), v1, 1));
    EXPECT_TRUE(session.series().HasSeries(session.txn(), e1, 0));
    EXPECT_FALSE(session.series().HasSeries(session.txn(), v2, 1));
    EXPECT_FALSE(session.series().HasSeries(session.txn(), e1, 7));

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v2, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].values[0].d, 3.0);

    // Dropping one field leaves the element's other series alone.
    session.series().DeleteField(session.txn(), v1, 0);
    session.Commit();
    EXPECT_FALSE(session.series().HasSeries(session.txn(), v1, 0));
    EXPECT_TRUE(session.series().HasSeries(session.txn(), v1, 1));

    // Deleting the element drops every field of it, and nothing else.
    session.series().DeleteElement(session.txn(), v1);
    session.Commit();
    EXPECT_FALSE(session.series().HasSeries(session.txn(), v1, 1));
    EXPECT_TRUE(session.series().HasSeries(session.txn(), v2, 0));
    EXPECT_TRUE(session.series().HasSeries(session.txn(), e1, 0));
    EXPECT_EQ(session.series().Table()->GetKeyCount(session.txn()), 2u);

    points.clear();
    ASSERT_TRUE(session.series().Range(session.txn(), e1, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].values[0].d, 4.0);
}

TEST_F(TestSeriesStore, AbortLeavesNoResidue) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(1);
    const std::vector<MeasureColumn> cols = OhlcColumns();

    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 0, Point5(1.0), cols,
                                        kDefaultPolicy));
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, kUsPerDay, Point5(2.0), cols,
                                        kDefaultPolicy));
    session.Abort();

    std::vector<Point> points;
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_TRUE(points.empty());
    EXPECT_FALSE(session.series().HasSeries(session.txn(), v, 0));
    session.Commit();

    // A second session on the same directory sees nothing either.
    session.Close();
    session.Open(dir_);
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_TRUE(points.empty());
}

TEST_F(TestSeriesStore, DataSurvivesReopenAndClose) {
    const ElementKey v = ElementKey::FromVertex(42);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    BucketPolicy policy;
    policy.max_points = 3;

    {
        SeriesSession session(dir_);
        for (int i = 0; i < 7; ++i) {
            ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, i * kUsPerDay, Point5(i), cols,
                                                policy));
        }
        session.Commit();
    }

    SeriesSession reopened(dir_);
    std::vector<Point> points;
    ASSERT_TRUE(reopened.series().Range(reopened.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    ASSERT_EQ(points.size(), 7u);
    EXPECT_EQ(points[6].ts, 6 * kUsPerDay);
    EXPECT_DOUBLE_EQ(points[6].values[0].d, 6.0);
    Point latest;
    ASSERT_TRUE(reopened.series().Latest(reopened.txn(), v, 0, cols, &latest));
    EXPECT_EQ(latest.ts, 6 * kUsPerDay);
    // 7 points with a cap of 3 were split into 3 + 3 + 1 buckets, and the split
    // survives the reopen.
    EXPECT_EQ(reopened.series().Table()->GetKeyCount(reopened.txn()), 3u);
}

TEST_F(TestSeriesStore, RejectsMalformedBuckets) {
    SeriesSession session(dir_);
    const ElementKey v = ElementKey::FromVertex(1);
    const std::vector<MeasureColumn> cols = OhlcColumns();
    ASSERT_TRUE(session.series().Upsert(session.txn(), v, 0, 0, Point5(1.0), cols,
                                        kDefaultPolicy));
    session.Commit();

    std::string key;
    SeriesStore::MakeBucketKey(v, 0, 0, &key);
    const std::string good =
        session.series().Table()->GetValue(session.txn(), Value::ConstRef(key)).AsString();
    ASSERT_GT(good.size(), 32u);

    std::vector<Point> points;
    size_t count = 0;
    Point point;
    auto store_value = [&](const std::string& value) {
        session.series().Table()->SetValue(session.txn(), Value::ConstRef(key),
                                           Value::ConstRef(value), true);
    };
    auto expect_rejected = [&]() {
        EXPECT_FALSE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
        EXPECT_FALSE(session.series().Count(session.txn(), v, 0, kMinTs, kMaxTs, cols, &count));
        EXPECT_FALSE(session.series().Latest(session.txn(), v, 0, cols, &point));
    };
    auto expect_rejected_by_value_reads = [&]() {
        EXPECT_FALSE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
        EXPECT_FALSE(session.series().Latest(session.txn(), v, 0, cols, &point));
    };

    // A value that is not a bucket at all.
    store_value("not a bucket at all");
    expect_rejected();

    // A corrupted format version.
    std::string bad_version = good;
    bad_version[4] = 9;
    store_value(bad_version);
    expect_rejected();

    // A corrupted magic.
    std::string bad_magic = good;
    bad_magic[0] = static_cast<char>(bad_magic[0] ^ 0x01);
    store_value(bad_magic);
    expect_rejected();

    // A chopped tail: the last column block is shorter than its own length. The
    // count still answers, because it only reads the header and the timestamp
    // column (that is what makes series.count cheap); the paths that touch the
    // measure columns notice.
    store_value(std::string(good.data(), good.size() - 1));
    expect_rejected_by_value_reads();
    ASSERT_TRUE(session.series().Count(session.txn(), v, 0, kMinTs, kMaxTs, cols, &count));
    EXPECT_EQ(count, 1u);

    // The intact value still decodes, so the failures above are the corruption
    // checks and not a broken setup.
    store_value(good);
    ASSERT_TRUE(session.series().Range(session.txn(), v, 0, kMinTs, kMaxTs, cols, &points));
    EXPECT_EQ(points.size(), 1u);
}

TEST_F(TestSeriesStore, EncodingRejectsInconsistentBuckets) {
    const std::vector<MeasureColumn> cols = OhlcColumns();
    Bucket bucket;
    bucket.first_ts = 0;
    bucket.measure_ids = {0, 1, 2, 3, 4};
    bucket.timestamps = {0, kUsPerDay};
    bucket.columns.assign(5, std::vector<MeasureValue>(2, D(1.0)));

    std::string encoded;
    ASSERT_TRUE(SeriesStore::EncodeBucket(bucket, cols, kDefaultPolicy, &encoded));
    Bucket decoded;
    ASSERT_TRUE(SeriesStore::DecodeBucket(encoded.data(), encoded.size(), cols, &decoded));
    EXPECT_EQ(decoded.timestamps, bucket.timestamps);
    EXPECT_EQ(decoded.measure_ids, bucket.measure_ids);

    // first_ts must equal the first point, timestamps must ascend, and every
    // column must have one value per point.
    Bucket bad = bucket;
    bad.first_ts = 99;
    EXPECT_FALSE(SeriesStore::EncodeBucket(bad, cols, kDefaultPolicy, &encoded));
    bad = bucket;
    bad.timestamps = {kUsPerDay, 0};
    EXPECT_FALSE(SeriesStore::EncodeBucket(bad, cols, kDefaultPolicy, &encoded));
    bad = bucket;
    bad.columns[3].pop_back();
    EXPECT_FALSE(SeriesStore::EncodeBucket(bad, cols, kDefaultPolicy, &encoded));
    bad = bucket;
    bad.measure_ids = {0, 1};
    EXPECT_FALSE(SeriesStore::EncodeBucket(bad, cols, kDefaultPolicy, &encoded));

    // The point cap is part of the value contract, so encoding refuses to
    // produce an over-full bucket instead of silently writing one.
    BucketPolicy tight;
    tight.max_points = 1;
    EXPECT_FALSE(SeriesStore::EncodeBucket(bucket, cols, tight, &encoded));

    // Decoding requires the exact measure set and rejects unknown column ids.
    std::vector<MeasureColumn> fewer(2, MeasureColumn{MeasureType::DOUBLE});
    EXPECT_FALSE(SeriesStore::DecodeBucket(encoded.data(), encoded.size(), fewer, &decoded));
    Bucket small;
    small.first_ts = 0;
    small.measure_ids = {0};
    small.timestamps = {0};
    small.columns.assign(1, std::vector<MeasureValue>(1, D(1.0)));
    std::vector<MeasureColumn> one_column(1, MeasureColumn{MeasureType::DOUBLE});
    ASSERT_TRUE(SeriesStore::EncodeBucket(small, one_column, kDefaultPolicy, &encoded));
    std::string tampered = encoded;
    tampered[0] = static_cast<char>(tampered[0] + 1);  // break the magic
    EXPECT_FALSE(SeriesStore::DecodeBucket(tampered.data(), tampered.size(), cols, &decoded));
    EXPECT_FALSE(SeriesStore::DecodeBucket(encoded.data(), encoded.size() / 2, cols, &decoded));
}
