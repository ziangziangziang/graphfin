// Minimal ASan/UBSan harness: deterministic adversarial decoder fuzz plus
// store round-trips, reusing the exact mutation battery from
// TestSeriesStore.AdversarialBucketMutationsNeverCrash at higher volume.
//
// Build (in tugraph-compile-arm64:phase0, from the repo root), reusing the
// ASan+UBSan-instrumented objects from build-asan; see REPORT.md for the
// object list and link flags.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/kv_store.h"
#include "core/lmdb_store.h"
#include "core/series_store.h"

using namespace lgraph;          // NOLINT
using namespace lgraph::series;  // NOLINT

static const int64_t kDay = 86400LL * 1000000LL;
static uint64_t g_state = 0x9E3779B97F4A7C15ULL;
static uint64_t Next() {
    g_state = g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_state >> 33;
}

int main(int argc, char** argv) {
    const int iters = argc > 1 ? atoi(argv[1]) : 20000;
    const std::string dir = "/tmp/asan_series_db";
    if (system(std::string("rm -rf " + dir).c_str()) != 0) return 1;
    LMDBKvStore store(dir, (size_t)1 << 30);
    auto setup = store.CreateWriteTxn();
    auto table = SeriesStore::OpenTable(*setup, store, SeriesStore::TableName());
    setup->Commit();
    auto txn0 = store.CreateWriteTxn();
    auto table0 = SeriesStore::OpenTable(*txn0, store, SeriesStore::TableName());
    SeriesStore series(*txn0, std::move(table0));
    txn0->Commit();

    // A valid multi-point bucket with mixed types and a null.
    Bucket bucket;
    bucket.first_ts = 0;
    bucket.measure_ids = {0, 1, 2};
    bucket.timestamps = {0, kDay, 2 * kDay, 3 * kDay, 4 * kDay};
    bucket.columns.assign(3, std::vector<MeasureValue>(5, MeasureValue::Double(1.25)));
    bucket.columns[2] = std::vector<MeasureValue>(5, MeasureValue::Int64(-42));
    bucket.columns[0][3] = MeasureValue::Null();
    std::vector<MeasureColumn> cols(3, MeasureColumn{MeasureType::DOUBLE});
    cols[2].type = MeasureType::INT64;
    std::string encoded;
    {
        auto txn = store.CreateWriteTxn();
        std::vector<MeasureValue> v = {MeasureValue::Double(1.25),
                                       MeasureValue::Double(1.25),
                                       MeasureValue::Int64(-42)};
        const ElementKey elem = ElementKey::FromVertex(3);
        for (int64_t t : bucket.timestamps) {
            if (!series.Upsert(*txn, elem, 0, t, v, cols, BucketPolicy())) {
                std::fprintf(stderr, "setup upsert failed\n");
                return 1;
            }
        }
        txn->Commit();
        std::string key;
        SeriesStore::MakeBucketKey(elem, 0, 0, &key);
        auto rt = store.CreateReadTxn();
        encoded = table->GetValue(*rt, Value::ConstRef(key)).AsString();
    }
    if (encoded.empty()) {
        std::fprintf(stderr, "no bucket stored\n");
        return 1;
    }

    Bucket decoded;
    int64_t first_ts = 0;
    std::vector<int64_t> ts_only;
    long decoded_ok = 0;
    for (int iter = 0; iter < iters; ++iter) {
        std::string mutated = encoded;
        switch (Next() % 6) {
        case 0: {
            size_t pos = Next() % mutated.size();
            mutated[pos] ^= static_cast<char>(1u << (Next() % 8));
            break;
        }
        case 1: {
            size_t pos = Next() % mutated.size();
            mutated[pos] = static_cast<char>(Next() % 256);
            break;
        }
        case 2:
            mutated.resize(Next() % (mutated.size() + 1));
            break;
        case 3: {
            static const uint32_t extremes[] = {0, 1, 2, 1000000u, 1000001u,
                                                0x7FFFFFFFu, 0xFFFFFFFFu};
            uint32_t v = extremes[Next() % 7];
            for (int b = 0; b < 4; ++b)
                mutated[8 + b] = static_cast<char>((v >> (8 * b)) & 0xFF);
            break;
        }
        case 4: {
            // Splice: duplicate a random slice at a random position.
            size_t at = Next() % mutated.size();
            size_t len = Next() % 64;
            mutated.insert(at, encoded.data(), std::min(len, encoded.size()));
            break;
        }
        default: {
            // Cross-bucket splice: glue two valid encodings together.
            mutated.append(encoded.data(), encoded.size());
            break;
        }
        }
        if (SeriesStore::DecodeBucket(mutated.data(), mutated.size(), cols, &decoded)) {
            ++decoded_ok;
        }
        SeriesStore::DecodeBucketTimestamps(mutated.data(), mutated.size(), &first_ts,
                                            &ts_only);
    }
    std::printf("ASAN-FUZZ-OK iters=%d decoded_ok=%ld\n", iters, decoded_ok);
    return 0;
}
