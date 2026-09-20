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

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "core/lightning_graph.h"
#include "core/series_store.h"
#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;          // NOLINT
using namespace lgraph_api;      // NOLINT
using namespace lgraph::series;  // NOLINT

namespace {

const int64_t kUsPerDay = 86400LL * 1000000LL;

FieldSpec SeriesField(const std::string& name) {
    FieldSpec f(name, FieldType::BLOB, true);
    f.series = true;
    f.series_spec.measures = {SeriesMeasureSpec{"open", FieldType::DOUBLE},
                              SeriesMeasureSpec{"close", FieldType::DOUBLE},
                              SeriesMeasureSpec{"volume", FieldType::INT64}};
    // Small enough that six points need more than one bucket.
    f.series_spec.bucket_max_points = 4;
    return f;
}

std::vector<MeasureValue> OnePoint(double open, double close, int64_t volume) {
    return {MeasureValue::Double(open), MeasureValue::Double(close), MeasureValue::Int64(volume)};
}

void AddCompanyLabel(LightningGraph& db) {
    db.AddLabel("Company",
                std::vector<FieldSpec>{FieldSpec("id", FieldType::INT64, false),
                                       SeriesField("prices")},
                true, VertexOptions("id"));
}

/** Buckets physically stored in the graph's series table. */
size_t SeriesKeyCount(LightningGraph& db, lgraph::Transaction& txn) {
    return db.GetSeriesStore()->Table()->GetKeyCount(txn.GetTxn());
}

}  // namespace

class TestSeriesTransaction : public TuGraphTest {};

TEST_F(TestSeriesTransaction, PointsRoundTripAndCorrectThroughTheTransaction) {
    const std::string dir = "./testdb_series_txn";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v1 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"1"});
    VertexId v2 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"2"});
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v1, "prices", i * kUsPerDay,
                                             OnePoint(100.0 + i, 101.0 + i, 1000 + i)));
    }
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v2, "prices", 0, OnePoint(200.0, 201.0, 5)));
    // A second write to the same timestamp corrects the point in place.
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v1, "prices", 2 * kUsPerDay, OnePoint(999.0, 1000.0, 7)));

    std::vector<series::Point> points;
    ASSERT_TRUE(txn.GetVertexSeriesRange(v1, "prices", kMinTs, kMaxTs, &points));
    ASSERT_EQ(points.size(), 6u);
    EXPECT_DOUBLE_EQ(points[2].values[0].d, 999.0);
    EXPECT_EQ(points[2].values[2].i, 7);
    EXPECT_DOUBLE_EQ(points[5].values[1].d, 106.0);

    size_t count = 0;
    ASSERT_TRUE(txn.GetVertexSeriesCount(v1, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 6u);
    ASSERT_TRUE(txn.GetVertexSeriesCount(v1, "prices", kUsPerDay, 2 * kUsPerDay, &count));
    EXPECT_EQ(count, 2u);

    series::Point point;
    ASSERT_TRUE(txn.GetVertexSeriesLatest(v1, "prices", &point));
    EXPECT_EQ(point.ts, 5 * kUsPerDay);
    ASSERT_TRUE(txn.GetVertexSeriesEarliest(v1, "prices", &point));
    EXPECT_EQ(point.ts, 0);

    // Each vertex owns its own series.
    ASSERT_TRUE(txn.GetVertexSeriesLatest(v2, "prices", &point));
    EXPECT_EQ(point.ts, 0);
    EXPECT_DOUBLE_EQ(point.values[0].d, 200.0);
    EXPECT_DOUBLE_EQ(point.values[2].i, 5);

    // 6 points at a cap of 4 buckets as 4 + 2, and v2 adds one more.
    EXPECT_EQ(SeriesKeyCount(db, txn), 3u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, SeriesSurvivesCommitAndReopen) {
    const std::string dir = "./testdb_series_txn_reopen";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    {
        LightningGraph db(conf);
        AddCompanyLabel(db);
        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                   std::vector<std::string>{"7"});
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                                 OnePoint(10.0 + i, 11.0 + i, i)));
        }
        txn.Commit();
    }
    {
        LightningGraph db(conf);
        auto txn = db.CreateReadTxn();
        auto it = txn.GetVertexIterator();
        ASSERT_TRUE(it.IsValid());  // the graph only holds the one vertex
        VertexId v = it.GetId();
        std::vector<series::Point> points;
        ASSERT_TRUE(txn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 3u);
        EXPECT_DOUBLE_EQ(points[2].values[0].d, 12.0);
        EXPECT_EQ(points[2].values[2].i, 2);
    }
}

TEST_F(TestSeriesTransaction, DeletingAVertexDropsItsBuckets) {
    const std::string dir = "./testdb_series_txn_delete";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v1 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"1"});
    VertexId v2 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"2"});
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v1, "prices", i * kUsPerDay, OnePoint(i, i, i)));
    }
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v2, "prices", 0, OnePoint(9.0, 9.0, 9)));
    ASSERT_EQ(SeriesKeyCount(db, txn), 3u);  // two buckets for v1, one for v2

    ASSERT_TRUE(txn.DeleteVertex(v1));
    EXPECT_EQ(SeriesKeyCount(db, txn), 1u);
    std::vector<series::Point> points;
    ASSERT_TRUE(txn.GetVertexSeriesRange(v2, "prices", kMinTs, kMaxTs, &points));
    EXPECT_EQ(points.size(), 1u);

    // Clearing one field leaves the rest of the element alone.
    ASSERT_TRUE(txn.DeleteVertex(v2));
    EXPECT_EQ(SeriesKeyCount(db, txn), 0u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, ClearSeriesAndRefuseOrdinaryWrites) {
    const std::string dir = "./testdb_series_txn_clear";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay, OnePoint(i, i, i)));
    }

    // A series field has no bytes in the record, so the ordinary property APIs
    // refuse it instead of quietly storing something the store will not see.
    // (The vectors are built outside the macro: a brace-list with a comma in it
    // would split the macro's argument list.)
    const std::vector<std::string> series_field{"prices"};
    const std::vector<std::string> one_value{"x"};
    UT_EXPECT_THROW_CODE(txn.SetVertexProperty(v, series_field, one_value), InputError);
    const std::vector<std::string> id_and_series{"id", "prices"};
    const std::vector<std::string> two_values{"2", "x"};
    UT_EXPECT_THROW_CODE(txn.AddVertex(std::string("Company"), id_and_series, two_values),
                         InputError);
    // The number of values has to match the measures the field declares.
    const std::vector<MeasureValue> too_few{MeasureValue::Double(1)};
    UT_EXPECT_THROW_CODE(txn.SetVertexSeriesPoint(v, "prices", 0, too_few), InputError);
    // Fields that exist but are not series, and fields that do not exist.
    UT_EXPECT_THROW_CODE(txn.ClearVertexSeries(v, "id"), InputError);
    EXPECT_FALSE(txn.ClearVertexSeries(v, "no_such_field"));
    EXPECT_FALSE(txn.SetVertexSeriesPoint(v, "no_such_field", 0, OnePoint(1, 1, 1)));

    size_t count = 0;
    ASSERT_TRUE(txn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 3u);
    ASSERT_TRUE(txn.ClearVertexSeries(v, "prices"));
    ASSERT_TRUE(txn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(SeriesKeyCount(db, txn), 0u);
    txn.Commit();

    // A read-only transaction cannot write points.
    auto rtxn = db.CreateReadTxn();
    UT_EXPECT_THROW_CODE(rtxn.SetVertexSeriesPoint(v, "prices", 0, OnePoint(1, 1, 1)),
                         WriteNotAllowed);
    UT_EXPECT_THROW_CODE(rtxn.ClearVertexSeries(v, "prices"), WriteNotAllowed);
}
