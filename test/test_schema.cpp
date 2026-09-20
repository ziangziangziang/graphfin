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

#include "fma-common/utils.h"
#include "gtest/gtest.h"

#include "core/schema.h"
#include "core/lmdb_transaction.h"
#include "lgraph/lgraph.h"
#include "./test_tools.h"
using namespace lgraph;
using namespace lgraph_api;

class TestSchema : public TuGraphTest, public testing::WithParamInterface<bool> {};

static Schema ConstructSimpleSchema(bool fast_alter) {
    Schema s;
    s.SetFastAlterSchema(fast_alter);
    s.SetSchema(true,
                std::vector<FieldSpec>({FieldSpec("int16", FieldType::INT16, false),
                                        FieldSpec("string", FieldType::STRING, true),
                                        FieldSpec("blob", FieldType::BLOB, true),
                                        FieldSpec("date", FieldType::DATE, false)}),
                "int16", "", {}, {});
    return s;
}

class InMemoryBlobManager {
    std::map<BlobManager::BlobKey, Value> map_;
    BlobManager::BlobKey next_key_ = 0;

 public:
    BlobManager::BlobKey Add(const Value& v) {
        map_[next_key_] = v.MakeCopy();
        return next_key_++;
    }

    Value Get(const BlobManager::BlobKey& bk) { return map_[bk]; }
};

TEST_P(TestSchema, LoadStoreSchema) {
    Schema s = ConstructSimpleSchema(GetParam());
    Value v = s.StoreSchema();
    Schema s2;
    s2.LoadSchema(v);
    UT_EXPECT_TRUE(s.GetFieldSpecsAsMap() == s2.GetFieldSpecsAsMap());
}

TEST_P(TestSchema, ConstructorsAndOperators) {
    Schema s = ConstructSimpleSchema(GetParam());
    UT_EXPECT_EQ(s.GetNumFields(), 4);
    Schema s2 = s;
    UT_EXPECT_EQ(s2.GetNumFields(), 4);
    UT_EXPECT_TRUE(s.GetFieldSpecsAsMap() == s2.GetFieldSpecsAsMap());
    Schema s3 = std::move(s2);
    UT_EXPECT_EQ(s3.GetNumFields(), 4);
    UT_EXPECT_TRUE(s.GetFieldSpecsAsMap() == s3.GetFieldSpecsAsMap());
    s2 = s3;
    UT_EXPECT_EQ(s2.GetNumFields(), 4);
    UT_EXPECT_TRUE(s.GetFieldSpecsAsMap() == s2.GetFieldSpecsAsMap());
    s3.ClearFields();
    UT_EXPECT_EQ(s3.GetNumFields(), 0);
}

TEST_P(TestSchema, SetSchema) {
    Schema s;
    UT_EXPECT_THROW_CODE(
        s.SetSchema(true,
                    std::vector<FieldSpec>({FieldSpec("int16", FieldType::INT16, true),
                                            FieldSpec("int16", FieldType::INT16, true)}),
                    "int16", "", {}, {}),
        FieldAlreadyExists);
    UT_EXPECT_THROW_CODE(
        s.SetSchema(true, std::vector<FieldSpec>({FieldSpec("int16", FieldType::NUL, true)}),
                    "int16", "", {}, {}),
        FieldCannotBeNullType);
    std::vector<FieldSpec> fs;
    for (size_t i = 0; i < _detail::MAX_NUM_FIELDS + 1; i++)
        fs.emplace_back(UT_FMT("f_{}", i), FieldType::INT16, true);
    UT_EXPECT_THROW_MSG(s.SetSchema(true, fs, "f_0", "", {}, {}), "Invalid Field");
}

TEST_P(TestSchema, HasBlob) {
    Schema s = ConstructSimpleSchema(GetParam());
    UT_EXPECT_TRUE(s.HasBlob());
    Schema s2 = s;
    UT_EXPECT_TRUE(s2.HasBlob());
    s.SetSchema(true, std::vector<FieldSpec>({FieldSpec("f", FieldType::INT16, true)}), "f", "", {},
                {});
    UT_EXPECT_TRUE(!s.HasBlob());
    s = s2;
    UT_EXPECT_TRUE(s.HasBlob());
    s.ClearFields();
    UT_EXPECT_TRUE(!s.HasBlob());
}

TEST_P(TestSchema, GetFieldExtractor) {
    Schema s = ConstructSimpleSchema(GetParam());
    for (auto& fs : s.GetFieldSpecs()) UT_EXPECT_TRUE(s.GetFieldExtractor(fs.name));
    for (size_t i = 0; i < s.GetNumFields(); i++) UT_EXPECT_TRUE(s.GetFieldExtractor(i));
    UT_EXPECT_THROW_CODE(s.GetFieldExtractor(s.GetNumFields()), FieldNotFound);
    UT_EXPECT_THROW_CODE(s.GetFieldExtractor("non-existing"), FieldNotFound);
}

TEST_P(TestSchema, GetFieldId) {
    Schema s = ConstructSimpleSchema(GetParam());
    std::vector<std::string> fnames;
    for (auto& fs : s.GetFieldSpecs()) {
        fnames.push_back(fs.name);
    }
    UT_EXPECT_EQ(fnames.size(), 4);
    std::reverse(fnames.begin(), fnames.end());
    std::vector<size_t> fids = s.GetFieldIds(fnames);
    for (size_t i = 0; i < fnames.size(); i++) {
        UT_EXPECT_EQ(s.GetFieldId(fnames[i]), fids[i]);
        size_t fid;
        UT_EXPECT_TRUE(s.TryGetFieldId(fnames[i], fid));
        UT_EXPECT_EQ(fid, fids[i]);
    }
    size_t fid;
    UT_EXPECT_TRUE(!s.TryGetFieldId("non-existing", fid));
}

TEST_P(TestSchema, DumpRecord) {
    Value v_old("name");
    Value v_new("name1");
    Schema schema(false);
    Schema schema_1(true);
    Schema schema_lg = schema;
    schema.SetFastAlterSchema(GetParam());
    schema_1.SetFastAlterSchema(GetParam());
    FieldSpec fd_0("name", FieldType::STRING, false);
    FieldSpec fd_1("uid", FieldType::INT32, false);
    FieldSpec fd_2("weight", FieldType::FLOAT, false);
    FieldSpec fd_3("age", FieldType::INT8, true);
    FieldSpec fd_4("addr", FieldType::STRING, true);
    FieldSpec fd_5("float", FieldType::DOUBLE, true);
    std::vector<FieldSpec> fds{fd_0, fd_1, fd_2, fd_3, fd_4, fd_5};
    schema.SetSchema(true, fds, "uid", "", {}, {});
    schema_1.SetSchema(true, fds, "uid", "", {}, {});
    UT_EXPECT_EQ(schema.GetNumFields(), 6);
    UT_LOG() << "size of schema:" << schema.GetNumFields();
    schema.SetSchema(true, fds, "uid", "", {}, {});
    Value va_tmp = schema.CreateEmptyRecord();
    UT_EXPECT_THROW_CODE(schema_1.SetField(va_tmp, (std::string) "name", FieldData()),
                         FieldCannotBeSetNull);
    UT_EXPECT_THROW(schema_1.SetField(va_tmp, (std::string) "age", FieldData(256)),
                    lgraph::ParseFieldDataException);
    UT_EXPECT_THROW_CODE(schema_1.SetField(va_tmp, (std::string) "name", FieldData(256)),
                         ParseIncompatibleType);
    UT_EXPECT_TRUE(schema_1.GetField(va_tmp, (std::string) "does_not_exist",
                                     [](const BlobManager::BlobKey&) { return Value(); }) ==
                   FieldData());

    // update index
    Value v_new_1("name");
    FieldData v_feild_str("weight");
    FieldData v_feild_int((int64_t)12);
    FieldData v_feild_real(12.01);
    FieldData v_feild_nul;
    UT_LOG() << "schema: " << fma_common::ToString(schema.GetFieldSpecs());
    {
        std::vector<size_t> fid = schema.GetFieldIds({"name", "uid", "weight", "age"});
        std::vector<std::string> value{"marko", "one", "80.2", "45"};
        // failed to parse uid
        UT_EXPECT_THROW(schema.CreateRecord(fid.size(), fid.data(), value.data()),
                        lgraph::ParseStringException);
    }
    {
        std::vector<size_t> fid = schema.GetFieldIds({"name", "uid"});
        std::vector<std::string> value{"marko", "300"};
        // missing weight field
        UT_EXPECT_THROW_CODE(schema.CreateRecord(fid.size(), fid.data(), value.data()),
                             FieldCannotBeSetNull);
    }

    std::vector<size_t> fid = schema.GetFieldIds({"name", "uid", "weight", "age", "addr"});
    std::vector<std::string> value{"peter", "101", "65.25", "49", "fifth avenue"};
    Value record = schema.CreateRecord(fid.size(), fid.data(), value.data());
    // UT_LOG() << "record: " << schema.DumpRecord(record);
    if (GetParam()) {
        UT_EXPECT_EQ(schema.GetFieldId("float"), 5);
    }
    UT_EXPECT_EQ(schema.GetFieldExtractor("name")->FieldToString(record), "peter");
    UT_EXPECT_EQ(schema.GetFieldExtractor("uid")->FieldToString(record), "101");
    UT_EXPECT_EQ(schema.GetFieldExtractor("weight")->FieldToString(record), "6.525e1");
    UT_EXPECT_EQ(schema.GetFieldExtractor("age")->FieldToString(record), "49");
    UT_EXPECT_EQ(schema.GetFieldExtractor("addr")->FieldToString(record), "fifth avenue");
    UT_EXPECT_THROW_CODE(schema.GetFieldExtractor("hash"), FieldNotFound);
    UT_EXPECT_THROW_CODE(schema.GetFieldExtractor(1024), FieldNotFound);
}

INSTANTIATE_TEST_SUITE_P(TestSchemaTest, TestSchema, testing::Values(true, false));

namespace {

// A series field: an optional BLOB carrying the series modifier.
FieldSpec MakeSeriesField(const std::string& name = "prices") {
    FieldSpec f(name, FieldType::BLOB, true);
    f.series = true;
    f.series_spec.measures = {SeriesMeasureSpec{"open", FieldType::DOUBLE},
                              SeriesMeasureSpec{"close", FieldType::DOUBLE},
                              SeriesMeasureSpec{"volume", FieldType::INT64}};
    f.series_spec.bucket_max_points = 500;
    f.series_spec.bucket_max_span_us = 2 * 86400ULL * 1000000ULL;
    f.series_spec.bucket_max_bytes = 4096;
    return f;
}

Schema MakeSchemaWith(bool fast_alter, const FieldSpec& series_field,
                      const std::string& primary = "id") {
    Schema s;
    s.SetFastAlterSchema(fast_alter);
    s.SetSchema(true,
                std::vector<FieldSpec>({FieldSpec("id", FieldType::INT32, false), series_field,
                                        FieldSpec("name", FieldType::STRING, true)}),
                primary, "", {}, {});
    return s;
}

}  // namespace

TEST_P(TestSchema, SeriesFieldRoundTripsThroughSchema) {
    Schema s = MakeSchemaWith(GetParam(), MakeSeriesField());
    Value v = s.StoreSchema();
    Schema s2;
    s2.LoadSchema(v);
    UT_EXPECT_TRUE(s.GetFieldSpecsAsMap() == s2.GetFieldSpecsAsMap());

    // GetFieldSpecsAsMap() returns a map by value, so keep it alive.
    const std::map<std::string, FieldSpec> specs = s2.GetFieldSpecsAsMap();
    const FieldSpec& in = specs.at("prices");
    UT_EXPECT_TRUE(in.series);
    UT_EXPECT_EQ(in.type, FieldType::BLOB);
    UT_EXPECT_TRUE(in.optional);
    UT_EXPECT_EQ(in.series_spec.measures.size(), 3);
    UT_EXPECT_EQ(in.series_spec.measures[0].name, "open");
    UT_EXPECT_EQ(in.series_spec.measures[0].type, FieldType::DOUBLE);
    UT_EXPECT_EQ(in.series_spec.measures[2].name, "volume");
    UT_EXPECT_EQ(in.series_spec.measures[2].type, FieldType::INT64);
    UT_EXPECT_EQ(in.series_spec.bucket_max_points, 500);
    UT_EXPECT_EQ(in.series_spec.bucket_max_span_us, 2 * 86400ULL * 1000000ULL);
    UT_EXPECT_EQ(in.series_spec.bucket_max_bytes, 4096);

    // Fields without the modifier are untouched by the extension.
    UT_EXPECT_FALSE(specs.at("id").series);
    UT_EXPECT_EQ(specs.at("id").series_spec.measures.size(), 0);
    UT_EXPECT_FALSE(specs.at("name").series);
}

TEST_P(TestSchema, SeriesInfoIsOnlyWrittenWhenASeriesExists) {
    // This is what keeps existing databases readable: the series block is
    // appended only when a field actually has one, so a schema without any
    // serializes to exactly the bytes it did before series fields existed, and
    // a dump that simply ends after the V2 block still loads.
    Schema plain = MakeSchemaWith(GetParam(), FieldSpec("prices", FieldType::BLOB, true));
    Schema with_series = MakeSchemaWith(GetParam(), MakeSeriesField());
    const size_t plain_size = plain.StoreSchema().Size();
    const size_t with_size = with_series.StoreSchema().Size();
    UT_EXPECT_TRUE(with_size > plain_size);
    UT_EXPECT_TRUE(plain_size > 0);

    Schema reloaded;
    reloaded.LoadSchema(plain.StoreSchema());
    UT_EXPECT_TRUE(reloaded.GetFieldSpecsAsMap() == plain.GetFieldSpecsAsMap());
    Schema reloaded_series;
    reloaded_series.LoadSchema(with_series.StoreSchema());
    UT_EXPECT_TRUE(reloaded_series.GetFieldSpecsAsMap() == with_series.GetFieldSpecsAsMap());
}

TEST_P(TestSchema, RejectsInvalidSeriesFields) {
    auto rejects = [this](const FieldSpec& f, const std::string& primary = "id") {
        UT_EXPECT_THROW_CODE(MakeSchemaWith(GetParam(), f, primary), InputError);
    };

    // A series field must be an optional BLOB with no default value...
    FieldSpec not_optional = MakeSeriesField();
    not_optional.optional = false;
    rejects(not_optional);

    FieldSpec wrong_type = MakeSeriesField();
    wrong_type.type = FieldType::STRING;
    rejects(wrong_type);

    FieldSpec with_default = MakeSeriesField();
    with_default.set_default_value = true;
    rejects(with_default);

    // ... it cannot be the primary field ...
    rejects(MakeSeriesField("id"));

    // ... and it must declare usable measures.
    FieldSpec no_measures = MakeSeriesField();
    no_measures.series_spec.measures.clear();
    rejects(no_measures);

    FieldSpec duplicate = MakeSeriesField();
    duplicate.series_spec.measures = {SeriesMeasureSpec{"open", FieldType::DOUBLE},
                                      SeriesMeasureSpec{"open", FieldType::INT64}};
    rejects(duplicate);

    FieldSpec bad_measure_type = MakeSeriesField();
    bad_measure_type.series_spec.measures = {SeriesMeasureSpec{"open", FieldType::STRING}};
    rejects(bad_measure_type);

    FieldSpec empty_measure_name = MakeSeriesField();
    empty_measure_name.series_spec.measures = {SeriesMeasureSpec{"", FieldType::DOUBLE}};
    rejects(empty_measure_name);

    FieldSpec too_many = MakeSeriesField();
    too_many.series_spec.measures.clear();
    for (size_t i = 0; i <= _detail::MAX_SERIES_MEASURES; ++i) {
        too_many.series_spec.measures.push_back(
            SeriesMeasureSpec{"m" + std::to_string(i), FieldType::DOUBLE});
    }
    rejects(too_many);

    FieldSpec no_point_cap = MakeSeriesField();
    no_point_cap.series_spec.bucket_max_points = 0;
    rejects(no_point_cap);

    FieldSpec no_byte_cap = MakeSeriesField();
    no_byte_cap.series_spec.bucket_max_bytes = 0;
    rejects(no_byte_cap);

    // Measures without the modifier are a DDL mistake, not a schema to store.
    FieldSpec unflagged = MakeSeriesField();
    unflagged.series = false;
    rejects(unflagged);

    // The same declaration without the modifier is perfectly fine.
    UT_EXPECT_NO_THROW(MakeSchemaWith(GetParam(), FieldSpec("prices", FieldType::BLOB, true)));
}

TEST_P(TestSchema, RejectsAnIndexOnASeriesField) {
    // Indexes are attached after a schema is built, so this checks the path
    // that keeps existing extractors: here the field already carries an index
    // when the layout is refreshed. An index created on a live label goes
    // through the existing "BLOB cannot be indexed" rule instead, since a
    // series field is always a BLOB.
    Schema s = MakeSchemaWith(GetParam(), MakeSeriesField());
    s.GetFieldExtractor("prices")->SetFullTextIndex(true);
    std::vector<FieldSpec> added{FieldSpec("extra", FieldType::INT32, true)};
    UT_EXPECT_THROW_CODE(s.AddFields(added), InputError);
    // The rejected call left the schema alone.
    UT_EXPECT_TRUE(s.TryGetFieldExtractor("extra") == nullptr);
}

TEST_P(TestSchema, SeriesModifierIsInvisibleToTheRecord) {
    // The whole point of declaring a series as a modified BLOB is that the
    // record keeps nothing for it: the same schema with and without the
    // modifier must lay out identical records, and an empty record must leave
    // the field unset rather than reserving bytes for points.
    Schema plain = MakeSchemaWith(GetParam(), FieldSpec("prices", FieldType::BLOB, true));
    Schema series = MakeSchemaWith(GetParam(), MakeSeriesField());
    UT_EXPECT_EQ(plain.CreateEmptyRecord().Size(), series.CreateEmptyRecord().Size());
    UT_EXPECT_EQ(plain.GetNumFields(), series.GetNumFields());

    Value record = series.CreateEmptyRecord();
    // An empty record leaves the series field unset, which is the only state it
    // can be in: the record has nowhere to keep a point.
    UT_EXPECT_TRUE(series.GetFieldExtractor("prices")->GetIsNull(record));
    UT_EXPECT_TRUE(plain.GetFieldExtractor("prices")->GetIsNull(plain.CreateEmptyRecord()));
}
