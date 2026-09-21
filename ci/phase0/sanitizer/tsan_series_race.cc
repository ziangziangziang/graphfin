// Minimal ThreadSanitizer harness for the series store's shared state.
//
// The full unit_test binary cannot run under TSan in this image: the
// prebuilt libvsag.so (OpenMP) preempts TSan's pthread interceptors at init.
// This harness links only the TSan-instrumented store objects, so the race
// detector actually runs over concurrent Range/Count/Latest/Upsert traffic
// on one shared SeriesStore - the exact R3 shape (plus writer overlap).
//
// Build (in tugraph-compile-arm64:phase0, from the repo root):
//   c++ -std=c++17 -g -fsanitize=thread -fno-omit-frame-pointer \
//     -I include -I src -I deps/geax-front-end/include \
//     /tmp/tsan_series_race.cc <objects...> -o /tmp/tsan_series_race \
//     -L/usr/local/lib64 -ltsan -lpthread
// Run (Docker blocks the ASLR personality syscall by default):
//   docker run --security-opt seccomp=unconfined ...

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/kv_store.h"
#include "core/lmdb_store.h"
#include "core/series_store.h"

using namespace lgraph;          // NOLINT
using namespace lgraph::series;  // NOLINT

static const int64_t kDay = 86400LL * 1000000LL;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/tsan_series_db";
    std::string rm = std::string("rm -rf ") + dir;
    if (system(rm.c_str()) != 0) return 1;
    // 1 GiB map: the default 4 TB reservation plus the TSan shadow
    // does not fit the container address budget.
    LMDBKvStore store(dir, (size_t)1 << 30);
    auto setup = store.CreateWriteTxn();
    auto table = SeriesStore::OpenTable(*setup, store, SeriesStore::TableName());
    setup->Commit();
    auto txn0 = store.CreateWriteTxn();
    auto table0 = SeriesStore::OpenTable(*txn0, store, SeriesStore::TableName());
    SeriesStore series(*txn0, std::move(table0));
    txn0->Commit();

    const ElementKey elem = ElementKey::FromVertex(7);
    const std::vector<MeasureColumn> cols(2, MeasureColumn{MeasureType::DOUBLE});
    const BucketPolicy policy;
    {
        auto txn = store.CreateWriteTxn();
        for (int i = 0; i < 16; ++i) {
            std::vector<MeasureValue> v = {MeasureValue::Double(i),
                                           MeasureValue::Double(2 * i)};
            if (!series.Upsert(*txn, elem, 0, i * kDay, v, cols, policy)) {
                std::fprintf(stderr, "setup upsert failed\n");
                return 1;
            }
        }
        txn->Commit();
    }

    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    auto reader = [&]() {
        for (int i = 0; i < 200; ++i) {
            auto txn = store.CreateReadTxn();
            std::vector<Point> points;
            size_t count = 0;
            Point latest;
            if (!series.Range(*txn, elem, 0, kMinTs, kMaxTs, cols, &points) ||
                !series.Count(*txn, elem, 0, kMinTs, kMaxTs, cols, &count) ||
                !series.Latest(*txn, elem, 0, cols, &latest) || points.size() != count) {
                failures.fetch_add(1);
                return;
            }
        }
    };
    auto writer = [&]() {
        for (int i = 0; i < 50 && failures.load() == 0; ++i) {
            auto txn = store.CreateWriteTxn();
            std::vector<MeasureValue> v = {MeasureValue::Double(1000 + i),
                                           MeasureValue::Double(i)};
            // New timestamps only: never invalidates a concurrent reader's
            // snapshot, but shares every byte of store state with it.
            series.Upsert(*txn, elem, 0, (100 + i) * kDay, v, cols, policy);
            txn->Commit();
        }
        stop.store(true);
    };
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) readers.emplace_back(reader);
    std::thread w(writer);
    w.join();
    for (auto& t : readers) t.join();
    if (failures.load() != 0) {
        std::fprintf(stderr, "FAILURES=%d\n", failures.load());
        return 1;
    }
    std::printf("TSAN-HARNESS-OK\n");
    return 0;
}
