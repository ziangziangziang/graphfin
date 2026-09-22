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
