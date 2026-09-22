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

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "./ut_config.h"
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

namespace {

FieldSpec IntSeriesField(const std::string& name) {
    FieldSpec f(name, FieldType::BLOB, true);
    f.series = true;
    f.series_spec.measures = {SeriesMeasureSpec{"v", FieldType::INT64}};
    return f;
}

void AddTwoSeriesLabel(LightningGraph& db, const std::string& label, bool fast_alter) {
    VertexOptions vo("id");
    vo.fast_alter_schema = fast_alter;
    UT_ASSERT(db.AddLabel(label,
                          std::vector<FieldSpec>{FieldSpec("id", FieldType::INT64, false),
                                                 IntSeriesField("prices"),
                                                 IntSeriesField("quotes")},
                          true, vo));
}

std::vector<MeasureValue> IntPoint(int64_t v) { return {MeasureValue::Int64(v)}; }

}  // namespace

TEST_F(TestSeriesTransaction, DeletingASeriesFieldLeavesSiblingSeriesIntact) {
    for (bool fast_alter : {false, true}) {
        const std::string dir =
            fast_alter ? "./testdb_series_txn_delfield_fast" : "./testdb_series_txn_delfield";
        AutoCleanDir cleaner(dir);
        DBConfig conf;
        conf.dir = dir;
        LightningGraph db(conf);
        AddTwoSeriesLabel(db, "Company", fast_alter);

        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                   std::vector<std::string>{"1"});
        const int64_t ts = 1234567890000000LL;
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", ts, IntPoint(11)));
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "quotes", ts, IntPoint(22)));
        txn.Commit();

        size_t n_modified = 0;
        ASSERT_TRUE(db.AlterLabelDelFields("Company", {"prices"}, true, &n_modified));

        auto rtxn = db.CreateReadTxn();
        auto it = rtxn.GetVertexIterator();
        ASSERT_TRUE(it.IsValid());
        VertexId rv = it.GetId();
        // The surviving field still returns its own data, not the deleted
        // field's: this is the P1-1 misassociation repro (returned 11 before).
        std::vector<series::Point> points;
        ASSERT_TRUE(rtxn.GetVertexSeriesRange(rv, "quotes", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].values[0].i, 22);
        // The deleted field is gone, with its buckets.
        EXPECT_FALSE(rtxn.GetVertexSeriesRange(rv, "prices", kMinTs, kMaxTs, &points));
        EXPECT_EQ(SeriesKeyCount(db, rtxn), 1u);
    }
}

TEST_F(TestSeriesTransaction, DeletingAPrecedingFieldMigratesSurvivingSeries) {
    const std::string dir = "./testdb_series_txn_delguard";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    VertexOptions vo("id");  // packed layout: field ids are positional
    UT_ASSERT(db.AddLabel("Company",
                          std::vector<FieldSpec>{FieldSpec("id", FieldType::INT64, false),
                                                 FieldSpec("note", FieldType::STRING, true),
                                                 IntSeriesField("prices")},
                          true, vo));

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, IntPoint(11)));
    txn.Commit();

    // Deleting "note" compacts "prices" from id 2 to id 1: its buckets move
    // with it in the same transaction instead of being orphaned or handed to
    // another field.
    size_t n_modified = 0;
    ASSERT_TRUE(db.AlterLabelDelFields("Company", {"note"}, true, &n_modified));

    auto rtxn = db.CreateReadTxn();
    std::vector<series::Point> points;
    ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].values[0].i, 11);
    EXPECT_EQ(SeriesKeyCount(db, rtxn), 1u);
}

TEST_F(TestSeriesTransaction, RedefiningSeriesMeasuresIsRejected) {
    const std::string dir = "./testdb_series_txn_modguard";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddTwoSeriesLabel(db, "Company", false);

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, IntPoint(11)));
    txn.Commit();

    // Buckets are decoded with the schema's measure set, so a redefinition
    // would orphan or misdecode them.
    FieldSpec redefined = IntSeriesField("prices");
    redefined.series_spec.measures = {SeriesMeasureSpec{"v2", FieldType::INT64}};
    UT_EXPECT_THROW_CODE(db.AlterLabelModFields("Company", {redefined}, true, nullptr),
                         InputError);
    FieldSpec unflagged("prices", FieldType::BLOB, true);
    UT_EXPECT_THROW_CODE(db.AlterLabelModFields("Company", {unflagged}, true, nullptr),
                         InputError);

    auto rtxn = db.CreateReadTxn();
    std::vector<series::Point> points;
    ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].values[0].i, 11);
}

TEST_F(TestSeriesTransaction, DropAllVertexClearsSeries) {
    const std::string dir = "./testdb_series_txn_dropall";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, OnePoint(1, 1, 1)));
    ASSERT_GT(SeriesKeyCount(db, txn), 0u);
    txn.Commit();

    db.DropAllVertex();
    auto rtxn = db.CreateReadTxn();
    EXPECT_EQ(SeriesKeyCount(db, rtxn), 0u);
}

TEST_F(TestSeriesTransaction, DelLabelClearsSeries) {
    const std::string dir = "./testdb_series_txn_dellabel";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, OnePoint(1, 1, 1)));
    ASSERT_GT(SeriesKeyCount(db, txn), 0u);
    txn.Commit();

    size_t n_modified = 0;
    ASSERT_TRUE(db.DelLabel("Company", true, &n_modified));
    auto rtxn = db.CreateReadTxn();
    EXPECT_EQ(SeriesKeyCount(db, rtxn), 0u);
}

TEST_F(TestSeriesTransaction, DeletingAVertexCleansIncidentEdgeSeries) {
    const std::string dir = "./testdb_series_txn_incedge";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    // An edge label with a single series field: in the packed layout its field
    // id is 0, which the count check below pins by reading the point back.
    UT_ASSERT(db.AddLabel("link", std::vector<FieldSpec>{IntSeriesField("w")}, false,
                          EdgeOptions()));

    auto txn = db.CreateWriteTxn();
    VertexId v0 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"1"});
    VertexId v1 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"2"});
    EdgeUid euid =
        txn.AddEdge(v0, v1, std::string("link"), std::vector<std::string>{},
                    std::vector<std::string>{});
    // No edge-level transaction API exists yet (S3), so write the bucket
    // through the store the transaction owns.
    std::vector<MeasureColumn> cols{MeasureColumn{MeasureType::INT64}};
    ASSERT_TRUE(db.GetSeriesStore()->Upsert(txn.GetTxn(), ElementKey::FromEdge(euid), 0, 0,
                                            IntPoint(7), cols, BucketPolicy()));
    size_t edge_count = 0;
    ASSERT_TRUE(db.GetSeriesStore()->Count(txn.GetTxn(), ElementKey::FromEdge(euid), 0, kMinTs,
                                           kMaxTs, cols, &edge_count));
    EXPECT_EQ(edge_count, 1u);

    // Vertex deletion only reaches incident edges through its callback, never
    // through DeleteEdge: their buckets have to go there too.
    ASSERT_TRUE(txn.DeleteVertex(v0));
    ASSERT_TRUE(db.GetSeriesStore()->Count(txn.GetTxn(), ElementKey::FromEdge(euid), 0, kMinTs,
                                           kMaxTs, cols, &edge_count));
    EXPECT_EQ(edge_count, 0u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, NarrowLookupsDoNotScanUnrelatedHistory) {
    const std::string dir = "./testdb_series_txn_scan";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);  // bucket_max_points = 4, so 30 points span ~8 buckets

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                             OnePoint(100.0 + i, 101.0 + i, 1000 + i)));
    }
    const size_t buckets = SeriesKeyCount(db, txn);
    ASSERT_GT(buckets, 4u);

    auto* store = db.GetSeriesStore();
    std::vector<series::Point> points;
    series::Point point;
    size_t count = 0;

    // A single-point lookup decodes its bucket plus the next bucket's header
    // (to prove the range ends), however long the history behind it is.
    store->ResetDecodeCount();
    ASSERT_TRUE(txn.GetVertexSeriesRange(v, "prices", 15 * kUsPerDay, 15 * kUsPerDay, &points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].values[0].d, 115.0);
    EXPECT_LE(store->DecodeCount(), 3u);

    // Latest/earliest read one bucket end each.
    store->ResetDecodeCount();
    ASSERT_TRUE(txn.GetVertexSeriesLatest(v, "prices", &point));
    EXPECT_EQ(point.ts, 29 * kUsPerDay);
    EXPECT_LE(store->DecodeCount(), 1u);

    store->ResetDecodeCount();
    ASSERT_TRUE(txn.GetVertexSeriesEarliest(v, "prices", &point));
    EXPECT_EQ(point.ts, 0);
    EXPECT_LE(store->DecodeCount(), 1u);

    // A narrow count only walks the buckets the window touches.
    store->ResetDecodeCount();
    ASSERT_TRUE(
        txn.GetVertexSeriesCount(v, "prices", 10 * kUsPerDay, 12 * kUsPerDay, &count));
    EXPECT_EQ(count, 3u);
    EXPECT_LE(store->DecodeCount(), buckets);

    // The summary is the documented exception: count/first/last genuinely
    // need the whole history, and the decode count shows it.
    store->ResetDecodeCount();
    series::SeriesSummary summary;
    ASSERT_TRUE(txn.ProbeVertexSeries(v, "prices", &summary));
    EXPECT_EQ(summary.count, 30u);
    EXPECT_GE(store->DecodeCount(), buckets);
    txn.Commit();
}

namespace {

void AddLinkLabel(LightningGraph& db) {
    UT_ASSERT(db.AddLabel("link", std::vector<FieldSpec>{IntSeriesField("w")}, false,
                          EdgeOptions()));
}

}  // namespace

TEST_F(TestSeriesTransaction, EdgeSeriesRoundTripCorrectAndClear) {
    const std::string dir = "./testdb_series_txn_edge";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    AddLinkLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v0 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"1"});
    VertexId v1 = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                std::vector<std::string>{"2"});
    EdgeUid euid = txn.AddEdge(v0, v1, std::string("link"), std::vector<std::string>{},
                               std::vector<std::string>{});
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(txn.SetEdgeSeriesPoint(euid, "w", i * kUsPerDay, IntPoint(10 + i)));
    }
    // In-place correction of one point.
    ASSERT_TRUE(txn.SetEdgeSeriesPoint(euid, "w", kUsPerDay, IntPoint(99)));

    std::vector<series::Point> points;
    ASSERT_TRUE(txn.GetEdgeSeriesRange(euid, "w", kMinTs, kMaxTs, &points));
    ASSERT_EQ(points.size(), 3u);
    EXPECT_EQ(points[1].values[0].i, 99);

    size_t count = 0;
    ASSERT_TRUE(txn.GetEdgeSeriesCount(euid, "w", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 3u);
    series::Point point;
    ASSERT_TRUE(txn.GetEdgeSeriesLatest(euid, "w", &point));
    EXPECT_EQ(point.ts, 2 * kUsPerDay);
    ASSERT_TRUE(txn.GetEdgeSeriesEarliest(euid, "w", &point));
    EXPECT_EQ(point.ts, 0);

    series::SeriesSummary summary;
    ASSERT_TRUE(txn.ProbeEdgeSeries(euid, "w", &summary));
    EXPECT_EQ(summary.count, 3u);
    EXPECT_TRUE(summary.has_points);
    ASSERT_EQ(summary.measures.size(), 1u);
    EXPECT_EQ(summary.measures[0].name, "v");

    // Missing edges and ordinary fields read as absent, not as errors.
    const EdgeUid no_such_edge(euid.dst, euid.src, euid.lid, euid.tid, euid.eid);
    EXPECT_FALSE(txn.GetEdgeSeriesRange(no_such_edge, "w", kMinTs, kMaxTs, &points));
    EXPECT_FALSE(txn.ClearEdgeSeries(euid, "no_such_field"));

    ASSERT_TRUE(txn.ClearEdgeSeries(euid, "w"));
    ASSERT_TRUE(txn.GetEdgeSeriesCount(euid, "w", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 0u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, RepeatUpsertIsByteIdentical) {
    const std::string dir = "./testdb_series_txn_idem";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);

    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                             OnePoint(100.0 + i, 101.0 + i, 1000 + i)));
    }
    auto snapshot = [&]() {
        std::vector<std::pair<std::string, std::string>> kvs;
        auto it = db.GetSeriesStore()->Table()->GetIterator(txn.GetTxn());
        it->GotoFirstKey();
        for (; it->IsValid(); it->Next()) {
            kvs.emplace_back(it->GetKey().AsString(), it->GetValue().AsString());
        }
        return kvs;
    };
    const auto before = snapshot();
    ASSERT_FALSE(before.empty());
    // Repeating every write verbatim must store exactly the same bytes.
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                             OnePoint(100.0 + i, 101.0 + i, 1000 + i)));
    }
    EXPECT_EQ(snapshot(), before);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, AddingFixedFieldThatWouldShiftSeriesIdsIsRejected) {
    // R1: packed-layout ids are positional, but reopen regroups fixed-width
    // fields before variable-width fields. Adding a fixed-width field behind a
    // series field (BLOB = variable-width) would orphan its buckets on the
    // next reopen, so the DDL is rejected before anything commits.
    const std::string dir = "./testdb_series_txn_r1guard";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    {
        LightningGraph db(conf);
        AddCompanyLabel(db);  // id INT64 (fixed) + prices series (variable)
        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                   std::vector<std::string>{"1"});
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, OnePoint(1.0, 2.0, 3)));
        txn.Commit();

        // A fixed-width field would sit before the series on reload: rejected.
        size_t n_modified = 0;
        UT_EXPECT_THROW_CODE(db.AlterLabelAddFields(
                                 "Company", {FieldSpec("extra", FieldType::INT64, true)},
                                 {FieldData()}, true, &n_modified),
                             InputError);
        // A variable-width field keeps the reload-canonical order: allowed, and
        // the series still reads back afterwards and after a reopen.
        ASSERT_TRUE(db.AlterLabelAddFields(
            "Company", {FieldSpec("note", FieldType::STRING, true)}, {FieldData()}, true,
            &n_modified));
        auto rtxn = db.CreateReadTxn();
        std::vector<series::Point> points;
        ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].values[2].i, 3);
    }
    {
        LightningGraph db(conf);
        auto rtxn = db.CreateReadTxn();
        auto it = rtxn.GetVertexIterator();
        ASSERT_TRUE(it.IsValid());
        std::vector<series::Point> points;
        ASSERT_TRUE(
            rtxn.GetVertexSeriesRange(it.GetId(), "prices", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].values[2].i, 3);
    }
}

TEST_F(TestSeriesTransaction, OrdinaryFieldCannotGainTheSeriesModifier) {
    // A populated ordinary field must not silently become a series: its stored
    // bytes would be hidden with no bucket migration.
    const std::string dir = "./testdb_series_txn_noseriesflip";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    UT_ASSERT(db.AddLabel("Company",
                          std::vector<FieldSpec>{FieldSpec("id", FieldType::INT64, false),
                                                 FieldSpec("note", FieldType::BLOB, true)},
                          true, VertexOptions("id")));
    FieldSpec as_series("note", FieldType::BLOB, true);
    as_series.series = true;
    as_series.series_spec.measures = {SeriesMeasureSpec{"v", FieldType::INT64}};
    UT_EXPECT_THROW_CODE(db.AlterLabelModFields("Company", {as_series}, true, nullptr),
                         InputError);
}

TEST_F(TestSeriesTransaction, FastAlterAddSeriesFieldSurvivesReopen) {
    // R2: a series added to an existing fast-alter label must persist a schema
    // that reopens. The lazy-add path used to stamp a default value that the
    // reopen validator rejects.
    const std::string dir = "./testdb_series_txn_r2fast";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    {
        LightningGraph db(conf);
        VertexOptions vo("id");
        vo.fast_alter_schema = true;
        UT_ASSERT(db.AddLabel("Company", {FieldSpec("id", FieldType::INT64, false)}, true,
                              vo));
        size_t n_modified = 0;
        ASSERT_TRUE(db.AlterLabelAddFields("Company", {IntSeriesField("prices")},
                                           {FieldData()}, true, &n_modified));
        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                   std::vector<std::string>{"1"});
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 0, IntPoint(42)));
        txn.Commit();
    }
    {
        LightningGraph db(conf);
        auto rtxn = db.CreateReadTxn();
        auto it = rtxn.GetVertexIterator();
        ASSERT_TRUE(it.IsValid());
        std::vector<series::Point> points;
        ASSERT_TRUE(
            rtxn.GetVertexSeriesRange(it.GetId(), "prices", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].values[0].i, 42);
    }
}

TEST_F(TestSeriesTransaction, DatabaseWithoutSeriesGainsItOnUpgrade) {
    // Old-version compatibility in miniature: a database whose labels predate
    // the series feature (ordinary fields only, no series table content) opens
    // fine, gains the series table on first open, accepts a lazily added
    // packed-layout series field, and keeps ordinary data plus points across
    // a further reopen.
    const std::string dir = "./testdb_series_txn_upgrade";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    {
        LightningGraph db(conf);
        UT_ASSERT(db.AddLabel("Company",
                              std::vector<FieldSpec>{
                                  FieldSpec("id", FieldType::INT64, false),
                                  FieldSpec("note", FieldType::STRING, true)},
                              true, VertexOptions("id")));
        auto txn = db.CreateWriteTxn();
        txn.AddVertex(std::string("Company"), std::vector<std::string>{"id", "note"},
                      std::vector<std::string>{"1", "old"});
        txn.Commit();
    }
    {
        LightningGraph db(conf);
        size_t n_modified = 0;
        ASSERT_TRUE(db.AlterLabelAddFields("Company", {IntSeriesField("prices")},
                                           {FieldData()}, true, &n_modified));
        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"),
                                   std::vector<std::string>{"id", "note"},
                                   std::vector<std::string>{"2", "new"});
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", 3 * kUsPerDay, IntPoint(7)));
        txn.Commit();
    }
    {
        LightningGraph db(conf);
        auto rtxn = db.CreateReadTxn();
        size_t vertices = 0;
        size_t points = 0;
        for (auto it = rtxn.GetVertexIterator(); it.IsValid(); it.Next()) {
            ++vertices;
            std::vector<series::Point> pts;
            ASSERT_TRUE(
                rtxn.GetVertexSeriesRange(it.GetId(), "prices", kMinTs, kMaxTs, &pts));
            points += pts.size();
            if (!pts.empty()) {
                EXPECT_EQ(pts[0].values[0].i, 7);
            }
        }
        EXPECT_EQ(vertices, 2u);
        EXPECT_EQ(points, 1u);
    }
}

TEST_F(TestSeriesTransaction, OutOfDateTimeRangeTimestampsAreRejected) {
    // R6: stored timestamps must round-trip through DATETIME on read. The
    // unbounded kMinTs/kMaxTs sentinels stay valid as range bounds but are
    // never valid stored points.
    const std::string dir = "./testdb_series_txn_tsbounds";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    UT_EXPECT_THROW_CODE(
        txn.SetVertexSeriesPoint(v, "prices", std::numeric_limits<int64_t>::max(),
                                 OnePoint(1.0, 1.0, 1)),
        InputError);
    UT_EXPECT_THROW_CODE(
        txn.SetVertexSeriesPoint(v, "prices", std::numeric_limits<int64_t>::min(),
                                 OnePoint(1.0, 1.0, 1)),
        InputError);
    ASSERT_TRUE(txn.SetVertexSeriesPoint(
        v, "prices", lgraph_api::MaxMicroSecondsSinceEpochForDateTime(),
        OnePoint(1.0, 1.0, 1)));
    ASSERT_TRUE(txn.SetVertexSeriesPoint(
        v, "prices", lgraph_api::MinMicroSecondsSinceEpochForDateTime(),
        OnePoint(2.0, 2.0, 2)));
    size_t count = 0;
    ASSERT_TRUE(txn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 2u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, NonFiniteDoublesAreRejectedAtWrite) {
    // R8: NaN/Infinity serialize as JSON null while staying non-null in the
    // engine, so writes refuse them at the transaction boundary (the Cypher
    // layer rejects them first). Actual nulls still write fine.
    const std::string dir = "./testdb_series_txn_nonfinite";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    auto txn = db.CreateWriteTxn();
    VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                               std::vector<std::string>{"1"});
    UT_EXPECT_THROW_CODE(
        txn.SetVertexSeriesPoint(v, "prices", 0,
                                 OnePoint(std::numeric_limits<double>::quiet_NaN(), 1.0, 1)),
        InputError);
    UT_EXPECT_THROW_CODE(
        txn.SetVertexSeriesPoint(v, "prices", 0,
                                 OnePoint(std::numeric_limits<double>::infinity(), 1.0, 1)),
        InputError);
    UT_EXPECT_THROW_CODE(
        txn.SetVertexSeriesPoint(
            v, "prices", 0, OnePoint(-std::numeric_limits<double>::infinity(), 1.0, 1)),
        InputError);
    // A null DOUBLE still stores as null (not an error).
    ASSERT_TRUE(txn.SetVertexSeriesPoint(
        v, "prices", 0, {MeasureValue::Null(), MeasureValue::Double(1.0),
                         MeasureValue::Int64(1)}));
    size_t count = 0;
    ASSERT_TRUE(txn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 1u);
    txn.Commit();
}

TEST_F(TestSeriesTransaction, PreSeriesReleaseFixtureUpgradesCleanly) {
    // Genuine old-version compatibility: data.mdb under
    // test/resource/data/preseries_db was written by lgraph_server at the
    // pre-series merge base (ordinary labels only, no series table, no series
    // schema block). The current build must open it, read every record, gain
    // the series table on first open, accept lazily added series fields, and
    // keep everything across a further reopen.
    const std::string dir = "./testdb_series_txn_preseries";
    AutoCleanDir cleaner(dir);
    const std::string fixture =
        lgraph::ut::TEST_RESOURCE_DIRECTORY + "/data/preseries_db/data.mdb";
    fma_common::file_system::MkDir(dir);
    UT_ASSERT(fma_common::FileSystem::GetFileSystem(fixture).CopyToLocal(
        fixture, dir + "/data.mdb"));
    DBConfig conf;
    conf.dir = dir;
    {
        LightningGraph db(conf);
        auto txn = db.CreateWriteTxn();
        size_t vertices = 0;
        int64_t id_sum = 0;
        size_t edges = 0;
        for (auto it = txn.GetVertexIterator(); it.IsValid(); it.Next()) {
            ++vertices;
            id_sum += txn.GetVertexField(it, std::string("id")).AsInt64();
            EXPECT_FALSE(txn.GetVertexField(it, std::string("name")).is_null());
            for (auto eit = txn.GetOutEdgeIterator(it.GetId()); eit.IsValid(); eit.Next()) {
                ++edges;
            }
        }
        EXPECT_EQ(vertices, 2u);
        EXPECT_EQ(id_sum, 3);
        EXPECT_EQ(edges, 1u);
        txn.Commit();
        size_t n_modified = 0;
        ASSERT_TRUE(db.AlterLabelAddFields("Company", {IntSeriesField("prices")},
                                           {FieldData()}, true, &n_modified));
    }
    VertexId v1 = 0;
    {
        LightningGraph db(conf);
        auto txn = db.CreateWriteTxn();
        for (auto it = txn.GetVertexIterator(); it.IsValid(); it.Next()) {
            if (txn.GetVertexField(it, std::string("id")).AsInt64() == 1) v1 = it.GetId();
        }
        ASSERT_TRUE(txn.SetVertexSeriesPoint(v1, "prices", 5 * kUsPerDay, IntPoint(11)));
        txn.Commit();
    }
    {
        LightningGraph db(conf);
        auto rtxn = db.CreateReadTxn();
        size_t vertices = 0;
        for (auto it = rtxn.GetVertexIterator(); it.IsValid(); it.Next()) ++vertices;
        EXPECT_EQ(vertices, 2u);
        std::vector<series::Point> points;
        ASSERT_TRUE(rtxn.GetVertexSeriesRange(v1, "prices", kMinTs, kMaxTs, &points));
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].values[0].i, 11);
    }
}

TEST_F(TestSeriesTransaction, ConcurrentReadersShareTheStoreSafely) {
    // R3: the decode counter is shared across transactions on one graph.
    // Concurrent readers must not race on it (exercised under TSan).
    const std::string dir = "./testdb_series_txn_concurrent";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    VertexId v = 0;
    {
        auto txn = db.CreateWriteTxn();
        v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                          std::vector<std::string>{"1"});
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                                 OnePoint(1.0 + i, 2.0 + i, i)));
        }
        txn.Commit();
    }
    constexpr int kReaders = 8;
    constexpr int kIters = 25;
    std::vector<std::thread> readers;
    std::atomic<int> failures{0};
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                auto rtxn = db.CreateReadTxn();
                std::vector<series::Point> points;
                if (!rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points) ||
                    points.size() != 8u) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : readers) t.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST_F(TestSeriesTransaction, ConcurrentReadersAndWriterOverlap) {
    // TSan target: readers ranging/counting while a writer appends new points.
    // The shared decode counter and bucket reads must stay race-free, and the
    // writer's commits must never corrupt a concurrent read.
    const std::string dir = "./testdb_series_txn_rw_overlap";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    VertexId v = 0;
    {
        auto txn = db.CreateWriteTxn();
        v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                          std::vector<std::string>{"1"});
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                                 OnePoint(1.0 + i, 2.0 + i, i)));
        }
        txn.Commit();
    }
    auto write_one_point = [&](int64_t ts, int64_t volume) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            try {
                auto txn = db.CreateWriteTxn();
                const bool ok = txn.SetVertexSeriesPoint(
                    v, "prices", ts, OnePoint(1.0 + volume, 2.0 + volume, volume));
                txn.Commit();
                return ok;
            } catch (const LgraphException& e) {
                if (e.code() != ErrorCode::TxnConflict) throw;
            }
        }
        return false;
    };
    constexpr int kExtra = 64;
    std::atomic<bool> writer_done{false};
    std::atomic<int> failures{0};
    std::thread writer([&]() {
        for (int i = 0; i < kExtra; ++i) {
            if (!write_one_point((8 + i) * kUsPerDay, 100 + i)) {
                failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    });
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            while (!writer_done.load(std::memory_order_acquire)) {
                auto rtxn = db.CreateReadTxn();
                std::vector<series::Point> points;
                size_t count = 0;
                series::Point latest;
                if (!rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points) ||
                    !rtxn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count) ||
                    !rtxn.GetVertexSeriesLatest(v, "prices", &latest) ||
                    points.size() != count || points.size() > 8u + kExtra) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    writer.join();
    for (auto& t : readers) t.join();
    EXPECT_EQ(failures.load(), 0);
    auto rtxn = db.CreateReadTxn();
    size_t count = 0;
    ASSERT_TRUE(rtxn.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 8u + kExtra);
    series::Point latest;
    ASSERT_TRUE(rtxn.GetVertexSeriesLatest(v, "prices", &latest));
    EXPECT_EQ(latest.ts, (8 + kExtra - 1) * kUsPerDay);
    EXPECT_EQ(latest.values[2].i, 100 + kExtra - 1);
}

TEST_F(TestSeriesTransaction, EightConcurrentWritersOnDistinctSeries) {
    // S5: one single-writer series per thread, the expected production shape.
    // No thread touches another's buckets, so every commit must succeed first
    // try and every series must hold exactly its own points.
    const std::string dir = "./testdb_series_txn_writers8";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    constexpr int kWriters = 8;
    constexpr int kPoints = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w]() {
            try {
                VertexId v = 0;
                {
                    auto txn = db.CreateWriteTxn();
                    v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                      std::vector<std::string>{std::to_string(100 + w)});
                    txn.Commit();
                }
                for (int batch = 0; batch < 5; ++batch) {
                    auto txn = db.CreateWriteTxn();
                    for (int i = 0; i < kPoints / 5; ++i) {
                        const int p = batch * (kPoints / 5) + i;
                        if (!txn.SetVertexSeriesPoint(v, "prices", p * kUsPerDay,
                                                      OnePoint(w * 1000.0 + p, p, p))) {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                    }
                    txn.Commit();
                }
            } catch (const std::exception&) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : writers) t.join();
    EXPECT_EQ(failures.load(), 0);
    // Every vertex holds exactly its own 50 points.
    auto rtxn = db.CreateReadTxn();
    size_t vertices = 0;
    for (auto vit = rtxn.GetVertexIterator(); vit.IsValid(); vit.Next()) {
        size_t count = 0;
        ASSERT_TRUE(rtxn.GetVertexSeriesCount(vit.GetId(), "prices", kMinTs, kMaxTs, &count));
        EXPECT_EQ(count, static_cast<size_t>(kPoints));
        ++vertices;
    }
    EXPECT_EQ(vertices, static_cast<size_t>(kWriters));
}

TEST_F(TestSeriesTransaction, SameSeriesContentionRetriesToFullCoverage) {
    // S5: several writers hammering one series conflict at LMDB level; each
    // retries its point until the commit lands. Disjoint timestamp blocks keep
    // the expected end state exact: every point present exactly once.
    const std::string dir = "./testdb_series_txn_same_series";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    VertexId v = 0;
    {
        auto txn = db.CreateWriteTxn();
        v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                          std::vector<std::string>{"1"});
        txn.Commit();
    }
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 40;
    std::atomic<int> failures{0};
    std::atomic<int64_t> commits{0};
    std::atomic<int64_t> conflicts{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w]() {
            try {
                for (int i = 0; i < kPerWriter; ++i) {
                    const int64_t p = w * kPerWriter + i;
                    bool done = false;
                    for (int attempt = 0; attempt < 500 && !done; ++attempt) {
                        try {
                            auto txn = db.CreateWriteTxn();
                            const bool ok = txn.SetVertexSeriesPoint(
                                v, "prices", p * kUsPerDay, OnePoint(p, p, p));
                            txn.Commit();
                            commits.fetch_add(1, std::memory_order_relaxed);
                            done = ok;
                        } catch (const LgraphException& e) {
                            if (e.code() != ErrorCode::TxnConflict) throw;
                            conflicts.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (!done) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            } catch (const std::exception&) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : writers) t.join();
    EXPECT_EQ(failures.load(), 0);
    UT_LOG() << "Same-series contention: " << commits.load() << " commits, "
             << conflicts.load() << " TxnConflict retries ("
             << (100.0 * conflicts.load() /
                 std::max<int64_t>(1, commits.load() + conflicts.load()))
             << "% abort rate)";
    auto rtxn = db.CreateReadTxn();
    std::vector<series::Point> points;
    ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &points));
    ASSERT_EQ(points.size(), static_cast<size_t>(kWriters * kPerWriter));
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(points[i].ts, static_cast<int64_t>(i) * kUsPerDay);
        EXPECT_EQ(points[i].values[2].i, static_cast<int64_t>(i));
    }
}

TEST_F(TestSeriesTransaction, RealisticOhlcvWorkload) {
    // S5: two trading years of daily bars for 500 symbols (250k points),
    // written one transaction per symbol. Asserts exact point counts, spot
    // values, single-bucket packing per symbol, and logs the stored
    // bytes-per-point against the 32-byte raw width (8B ts + 3x8B measures).
    const std::string dir = "./testdb_series_txn_ohlcv";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    FieldSpec prices = SeriesField("prices");
    prices.series_spec.bucket_max_points = 1000;
    UT_ASSERT(db.AddLabel("Company",
                          std::vector<FieldSpec>{FieldSpec("id", FieldType::INT64, false),
                                                 prices},
                          true, VertexOptions("id")));
    constexpr int kSymbols = 500;
    constexpr int kDays = 500;
    const auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < kSymbols; ++s) {
        auto txn = db.CreateWriteTxn();
        VertexId v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                                   std::vector<std::string>{std::to_string(s)});
        for (int d = 0; d < kDays; ++d) {
            ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", d * kUsPerDay,
                                                 OnePoint(100.0 + s + d * 0.1,
                                                          101.0 + s + d * 0.1, 1000 + d)));
        }
        txn.Commit();
    }
    const auto t1 = std::chrono::steady_clock::now();
    auto rtxn = db.CreateReadTxn();
    size_t vertices = 0;
    for (auto vit = rtxn.GetVertexIterator(); vit.IsValid(); vit.Next()) {
        size_t count = 0;
        ASSERT_TRUE(rtxn.GetVertexSeriesCount(vit.GetId(), "prices", kMinTs, kMaxTs, &count));
        EXPECT_EQ(count, static_cast<size_t>(kDays));
        ++vertices;
        if (vertices == 1 || vertices == kSymbols) {
            series::Point latest;
            ASSERT_TRUE(rtxn.GetVertexSeriesLatest(vit.GetId(), "prices", &latest));
            EXPECT_EQ(latest.ts, (kDays - 1) * kUsPerDay);
        }
    }
    EXPECT_EQ(vertices, static_cast<size_t>(kSymbols));
    EXPECT_EQ(SeriesKeyCount(db, rtxn), static_cast<size_t>(kSymbols));  // one bucket each
    size_t stored_bytes = 0;
    {
        auto it = db.GetSeriesStore()->Table()->GetIterator(rtxn.GetTxn());
        it->GotoFirstKey();
        for (; it->IsValid(); it->Next()) stored_bytes += it->GetValue().Size();
    }
    const double bytes_per_point =
        static_cast<double>(stored_bytes) / (kSymbols * kDays);
    UT_LOG() << "OHLCV: " << kSymbols * kDays << " points in "
             << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
             << "ms, " << bytes_per_point << " stored bytes/point vs 32 raw";
    EXPECT_LT(bytes_per_point, 32.0);
}

TEST_F(TestSeriesTransaction, OpenReadSurvivesConcurrentCommit) {
    // Transaction/store lifetimes: a read txn with live results stays valid
    // across another txn's commit, keeps its snapshot (no leaking of the new
    // points), and a fresh txn observes the committed state.
    const std::string dir = "./testdb_series_txn_snapshot";
    AutoCleanDir cleaner(dir);
    DBConfig conf;
    conf.dir = dir;
    LightningGraph db(conf);
    AddCompanyLabel(db);
    VertexId v = 0;
    {
        auto txn = db.CreateWriteTxn();
        v = txn.AddVertex(std::string("Company"), std::vector<std::string>{"id"},
                          std::vector<std::string>{"1"});
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(txn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                                 OnePoint(i, i, i)));
        }
        txn.Commit();
    }
    auto rtxn = db.CreateReadTxn();
    std::vector<series::Point> before;
    ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &before));
    ASSERT_EQ(before.size(), 4u);
    // A concurrent commit on another thread: LightningGraph forbids two live
    // transactions on one thread, so the writer runs separately.
    std::thread writer([&]() {
        auto wtxn = db.CreateWriteTxn();
        for (int i = 4; i < 8; ++i) {
            EXPECT_TRUE(wtxn.SetVertexSeriesPoint(v, "prices", i * kUsPerDay,
                                                  OnePoint(i, i, i)));
        }
        wtxn.Commit();
    });
    writer.join();
    // The held-open read still serves its snapshot; its earlier results are
    // untouched by the concurrent commit. The read ends before a fresh one
    // begins (one live transaction per thread).
    {
        std::vector<series::Point> during;
        ASSERT_TRUE(rtxn.GetVertexSeriesRange(v, "prices", kMinTs, kMaxTs, &during));
        ASSERT_EQ(during.size(), 4u);
        ASSERT_EQ(during.size(), before.size());
        for (size_t i = 0; i < 4u; ++i) {
            EXPECT_EQ(during[i].ts, before[i].ts);
            EXPECT_EQ(during[i].values[2].i, static_cast<int64_t>(i));
        }
    }
    rtxn.Abort();
    auto fresh = db.CreateReadTxn();
    size_t count = 0;
    ASSERT_TRUE(fresh.GetVertexSeriesCount(v, "prices", kMinTs, kMaxTs, &count));
    EXPECT_EQ(count, 8u);
}
