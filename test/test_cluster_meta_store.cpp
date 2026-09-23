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
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "core/kv_store.h"
#include "cluster/cluster_meta_store.h"

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestClusterMetaStore : public TuGraphTest {};

namespace {

std::unique_ptr<ClusterMetaStore> OpenStore(LMDBKvStore* store) {
    auto ms = std::make_unique<ClusterMetaStore>();
    auto txn = store->CreateWriteTxn(false);
    ms->Init(store, *txn, true);
    txn->Commit();
    return ms;
}

ShardInfo MakeShard(ShardId id) {
    ShardInfo s;
    s.shard_id = id;
    s.state = ShardState::ONLINE;
    s.name = "shard-" + std::to_string(id);
    s.endpoints = {"127.0.0.1:29092", "127.0.0.1:29093", "127.0.0.1:29094"};
    return s;
}

}  // namespace

TEST_F(TestClusterMetaStore, BasicCrud) {
    AutoCleanDir cleaner("./test_cluster_meta");
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta");
        auto ms = OpenStore(store.get());

        EXPECT_EQ(ms->GraphCount(), 0u);
        EXPECT_EQ(ms->ShardCount(), 0u);

        // Register two shards.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.RegisterShard(MakeShard(0)));
            EXPECT_TRUE(batch.RegisterShard(MakeShard(1)));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_EQ(ms->ShardCount(), 2u);
        ShardInfo got;
        EXPECT_TRUE(ms->GetShard(0, &got));
        EXPECT_EQ(got.name, "shard-0");
        EXPECT_EQ(got.endpoints.size(), 3u);
        EXPECT_TRUE(got.IsUsable());

        // Place graphs.
        ConfigVersion v0 = ms->Version();
        PlacementVersion pv = 0;
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &pv));
            EXPECT_TRUE(batch.PutGraphPlacement("g2", 1, PlacementState::ACTIVE));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_EQ(ms->GraphCount(), 2u);
        EXPECT_GT(pv, 0u);
        EXPECT_GT(ms->Version(), v0);

        GraphPlacement p;
        GraphId id1 = 0;
        EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id1));
        EXPECT_EQ(p.shard_id, 0);
        EXPECT_EQ(p.State(), PlacementState::ACTIVE);
        std::string name;
        EXPECT_TRUE(ms->GetGraphName(id1, &name));
        EXPECT_EQ(name, "g1");
        EXPECT_TRUE(ms->HasGraph("g2"));
        EXPECT_FALSE(ms->HasGraph("nope"));
        EXPECT_EQ(ms->GraphCountOnShard(0), 1u);

        // Updating an existing graph reuses its id and bumps its version.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            PlacementVersion pv2 = 0;
            EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::MOVING, &pv2));
            EXPECT_TRUE(batch.Commit());
            EXPECT_GT(pv2, pv);
        }
        EXPECT_EQ(ms->GraphCount(), 2u);
        GraphId id1b = 99;
        EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id1b));
        EXPECT_EQ(id1b, id1);
        EXPECT_EQ(p.shard_id, 1);
        EXPECT_EQ(p.State(), PlacementState::MOVING);

        // Removing a shard that hosts live graphs is refused.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_FALSE(batch.RemoveShard(1));
            batch.Abort();
        }
        // Delete g2; g1 is still on shard 1, so removal is still refused.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.DeleteGraphPlacement("g2"));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_FALSE(ms->HasGraph("g2"));
        EXPECT_EQ(ms->GraphCount(), 1u);
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_FALSE(batch.RemoveShard(1));
            batch.Abort();
        }
        // Move g1 back to shard 0; now shard 1 is empty and removable.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
            EXPECT_TRUE(batch.Commit());
        }
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.RemoveShard(1));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_EQ(ms->ShardCount(), 1u);
    }

    // Reload from disk: data and dense ids survive.
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta");
        auto ms = OpenStore(store.get());
        EXPECT_EQ(ms->GraphCount(), 1u);  // g2 deleted
        EXPECT_EQ(ms->ShardCount(), 1u);
        GraphPlacement p;
        GraphId id = 0;
        EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id));
        EXPECT_EQ(p.shard_id, 0);
        std::string name;
        EXPECT_TRUE(ms->GetGraphName(id, &name));
        EXPECT_EQ(name, "g1");

        // New graphs get dense ids after reload.
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g3", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
        EXPECT_EQ(ms->GraphCount(), 2u);
        EXPECT_TRUE(ms->GetGraphPlacement("g3", &p, &id));
        EXPECT_EQ(id, 1u);
    }
}

TEST_F(TestClusterMetaStore, VersionMonotonic) {
    AutoCleanDir cleaner("./test_cluster_meta_ver");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_ver");
    auto ms = OpenStore(store.get());

    ConfigVersion v = ms->Version();
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.RegisterShard(MakeShard(0)));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(ms->Version(), v);
    v = ms->Version();

    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("a", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(ms->Version(), v);

    // Explicit set is honoured and persists.
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.SetVersion(12345));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(ms->Version(), 12345u);
}

TEST_F(TestClusterMetaStore, StagingIsolationAndRollback) {
    AutoCleanDir cleaner("./test_cluster_meta_stage");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_stage");
    auto ms = OpenStore(store.get());

    // A staged, uncommitted placement must NOT be visible to readers.
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        PlacementVersion pv = 0;
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &pv));
        EXPECT_GT(batch.StagedCount(), 0u);
        EXPECT_EQ(batch.StagedCount(), 1u);
        GraphPlacement p;
        GraphId id = 0;
        EXPECT_FALSE(ms->GetGraphPlacement("g1", &p, &id));  // not published
        EXPECT_FALSE(ms->HasGraph("g1"));
        EXPECT_EQ(ms->GraphCount(), 0u);
        // Abort and roll back: no divergence between memory and storage.
        batch.Abort();
        EXPECT_EQ(batch.StagedCount(), 0u);
    }
    EXPECT_FALSE(ms->HasGraph("g1"));
    EXPECT_EQ(ms->GraphCount(), 0u);

    // Commit + publish makes the placement visible.
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement p;
    GraphId id = 0;
    EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id));
    EXPECT_EQ(ms->GraphCount(), 1u);

    // A batch publishes atomically: neither graph is visible until Commit.
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g2", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.PutGraphPlacement("g3", 0, PlacementState::ACTIVE));
        EXPECT_EQ(batch.StagedCount(), 2u);
        EXPECT_FALSE(ms->HasGraph("g2"));
        EXPECT_FALSE(ms->HasGraph("g3"));
        EXPECT_EQ(ms->GraphCount(), 1u);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_TRUE(ms->HasGraph("g2"));
    EXPECT_TRUE(ms->HasGraph("g3"));
    EXPECT_EQ(ms->GraphCount(), 3u);
    // TODO(graphfin): old global HasStaged() check removed; per-Batch staging is gone after Commit.
}

// A reader on another thread must observe only committed state while a writer
// has staged (but not published) an update.
TEST_F(TestClusterMetaStore, ConcurrentReaderSeesCommittedOnly) {
    AutoCleanDir cleaner("./test_cluster_meta_conc");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_conc");
    auto ms = OpenStore(store.get());

    PlacementVersion v1 = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.RegisterShard(MakeShard(0)));
        EXPECT_TRUE(batch.RegisterShard(MakeShard(1)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &v1));
        EXPECT_TRUE(batch.Commit());
    }

    // Stage a move to shard 1 but do NOT publish it.
    PlacementVersion v2 = 0;
    auto txn = store->CreateWriteTxn(false);
    ClusterMetaStore::Batch batch(ms.get(), *txn);
    EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::MOVING, &v2));
    EXPECT_GT(v2, v1);

    // A concurrent reader must still see the committed placement (shard 0).
    GraphPlacement seen;
    GraphId seen_id = 0;
    bool found = false;
    std::thread reader([&]() { found = ms->GetGraphPlacement("g1", &seen, &seen_id); });
    reader.join();
    EXPECT_TRUE(found);
    EXPECT_EQ(seen.shard_id, 0u);
    EXPECT_EQ(seen.placement_version, v1);
    EXPECT_EQ(seen.State(), PlacementState::ACTIVE);

    // Abort the staged move: state is unchanged.
    batch.Abort();
    GraphPlacement after;
    EXPECT_TRUE(ms->GetGraphPlacement("g1", &after, &seen_id));
    EXPECT_EQ(after.shard_id, 0u);
    EXPECT_EQ(after.placement_version, v1);
}

// The immutable graph identity must be assigned once, preserved across moves,
// and stable across a reload (unlike the dense, process-local GraphId).
TEST_F(TestClusterMetaStore, UniqueIdIsImmutableAcrossReload) {
    AutoCleanDir cleaner("./test_cluster_meta_uid");
    uint64_t uid1 = 0;
    uint64_t uid2 = 0;
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_uid");
        auto ms = OpenStore(store.get());
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, nullptr, &uid1));
        EXPECT_TRUE(batch.PutGraphPlacement("g2", 0, PlacementState::ACTIVE, nullptr, &uid2));
        EXPECT_TRUE(batch.Commit());
        EXPECT_GT(uid1, 0u);
        EXPECT_NE(uid1, uid2);
    }
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_uid");
        auto ms = OpenStore(store.get());
        GraphPlacement p;
        GraphId id = 0;
        EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id));
        EXPECT_EQ(p.unique_id, uid1);  // stable across reload

        // Moving the graph preserves its identity.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::ACTIVE));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id));
        EXPECT_EQ(p.unique_id, uid1);
        EXPECT_EQ(p.shard_id, 1u);

        // A newly created graph gets a strictly larger identity (no reuse).
        uint64_t uid3 = 0;
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g3", 0, PlacementState::ACTIVE, nullptr, &uid3));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_GT(uid3, uid2);
    }
}

TEST_F(TestClusterMetaStore, MemoryFootprintIsCompact) {
    AutoCleanDir cleaner("./test_cluster_meta_mem");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_mem");
    auto ms = OpenStore(store.get());

    const size_t N = 20000;
    {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        for (size_t i = 0; i < N; i++) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "graph_%06zu", i);
            EXPECT_TRUE(batch.PutGraphPlacement(buf, static_cast<ShardId>(i % 8),
                                               PlacementState::ACTIVE));
        }
        EXPECT_TRUE(batch.Commit());
    }

    EXPECT_EQ(ms->GraphCount(), N);
    size_t fp = ms->MemoryFootprint();
    UT_LOG() << "in-memory cluster index: " << fp << " bytes for " << N << " graphs = "
             << (fp / N) << " B/graph";
    // A naive unordered_map<string, struct> costs well over 100 B/graph. The
    // compact index must stay comfortably below that.
    EXPECT_LT(fp, N * 100);

    // Spot-check a mid-range lookup resolves to the right id/name.
    GraphPlacement p;
    GraphId id = 0;
    EXPECT_TRUE(ms->GetGraphPlacement("graph_012345", &p, &id));
    EXPECT_EQ(id, 12345u);
    std::string name;
    EXPECT_TRUE(ms->GetGraphName(id, &name));
    EXPECT_EQ(name, "graph_012345");
}

// R1: transaction-owned staging. While batch A holds staged (uncommitted)
// mutations, a second batch B must block (writer-serialized) rather than share
// staged state; A's commit publishes only A's ops, and B's abort publishes
// nothing.
TEST_F(TestClusterMetaStore, StagedBatchesAreTransactionOwned) {
    AutoCleanDir cleaner("./test_cluster_meta_r1");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_r1");
    auto ms = OpenStore(store.get());

    // Batch A stages g_a but does not commit yet (holds the writer lock).
    auto txn_a = store->CreateWriteTxn(false);
    ClusterMetaStore::Batch batch_a(ms.get(), *txn_a);
    EXPECT_TRUE(batch_a.PutGraphPlacement("g_a", 0, PlacementState::ACTIVE));

    // B tries to start while A is active: must not proceed until A finishes.
    // Use try semantics via a thread that records when it acquired the batch.
    std::atomic<bool> b_acquired{false};
    std::atomic<bool> b_done{false};
    std::thread b([&]() {
        auto txn_b = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch_b(ms.get(), *txn_b);
        b_acquired = true;
        // By now A has committed, so B must see A's graph (no shared staging,
        // but committed state is visible).
        EXPECT_TRUE(batch_b.HasGraph("g_a"));
        EXPECT_TRUE(batch_b.PutGraphPlacement("g_b", 0, PlacementState::ACTIVE));
        batch_b.Abort();  // g_b must never become visible
        b_done = true;
    });
    // Give B a chance to (incorrectly) proceed while A is still active.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(b_acquired) << "second batch must block while first batch is active";
    EXPECT_FALSE(ms->HasGraph("g_a"));  // A not yet committed: invisible
    EXPECT_TRUE(batch_a.Commit());
    EXPECT_TRUE(ms->HasGraph("g_a"));
    b.join();
    EXPECT_TRUE(b_acquired);
    EXPECT_TRUE(b_done);
    EXPECT_TRUE(ms->HasGraph("g_a"));
    EXPECT_FALSE(ms->HasGraph("g_b")) << "aborted batch must never publish";
}

// R10: SetShardState publishes the full descriptor (incl. config_version).
// ONLINE -> OFFLINE -> ONLINE via SetShardState only must converge in memory
// and after reload, preserving endpoints.
TEST_F(TestClusterMetaStore, ShardStatePublishesFullDescriptor) {
    AutoCleanDir cleaner("./test_cluster_meta_r10");
    uint64_t v_online = 0;
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_r10");
        auto ms = OpenStore(store.get());
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            ShardInfo s = MakeShard(0);
            s.endpoints = {"127.0.0.1:29092"};
            EXPECT_TRUE(batch.RegisterShard(s));
            EXPECT_TRUE(batch.Commit());
        }
        ShardInfo before;
        EXPECT_TRUE(ms->GetShard(0, &before));
        v_online = before.config_version;
        EXPECT_EQ(before.state, ShardState::ONLINE);

        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.SetShardState(0, ShardState::OFFLINE));
            EXPECT_TRUE(batch.Commit());
        }
        ShardInfo off;
        EXPECT_TRUE(ms->GetShard(0, &off));
        EXPECT_EQ(off.state, ShardState::OFFLINE);
        EXPECT_GT(off.config_version, v_online);
        EXPECT_EQ(off.endpoints, before.endpoints);

        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.SetShardState(0, ShardState::ONLINE));
            EXPECT_TRUE(batch.Commit());
        }
        ShardInfo on;
        EXPECT_TRUE(ms->GetShard(0, &on));
        EXPECT_EQ(on.state, ShardState::ONLINE);
        EXPECT_GT(on.config_version, off.config_version);
        EXPECT_EQ(on.endpoints, before.endpoints);
    }
    // Reload: durable and memory must agree (R10 divergence would show here).
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_r10");
        auto ms = OpenStore(store.get());
        ShardInfo re;
        EXPECT_TRUE(ms->GetShard(0, &re));
        EXPECT_EQ(re.state, ShardState::ONLINE);
        EXPECT_GT(re.config_version, v_online);
        EXPECT_EQ(re.endpoints.size(), 1u);
        EXPECT_EQ(re.endpoints[0], "127.0.0.1:29092");
    }
}

// R3: every incarnation gets a fresh UID — cross-batch, same-batch, and
// across restarts. A tombstone never donates its UID to the next incarnation.
TEST_F(TestClusterMetaStore, RecreateAllocatesFreshUid) {
    AutoCleanDir cleaner("./test_cluster_meta_r3");
    uint64_t uid1 = 0;
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_r3");
        auto ms = OpenStore(store.get());
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g", 0, PlacementState::ACTIVE, nullptr, &uid1));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_GT(uid1, 0u);
        // Delete then recreate in separate batches: fresh UID.
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.DeleteGraphPlacement("g"));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_FALSE(ms->HasGraph("g"));
        uint64_t uid2 = 0;
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g", 0, PlacementState::ACTIVE, nullptr, &uid2));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_NE(uid2, uid1);
        EXPECT_GT(uid2, uid1);
        // Delete + recreate inside ONE batch: still a fresh incarnation.
        uint64_t uid3 = 0;
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.DeleteGraphPlacement("g"));
            EXPECT_TRUE(batch.PutGraphPlacement("g", 1, PlacementState::ACTIVE, nullptr, &uid3));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_NE(uid3, uid2);
        EXPECT_GT(uid3, uid2);
        GraphPlacement p;
        EXPECT_TRUE(ms->GetGraphPlacement("g", &p));
        EXPECT_EQ(p.unique_id, uid3);
        EXPECT_EQ(p.shard_id, 1u);
    }
    // After a restart the allocator still exceeds every UID ever issued.
    {
        auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_r3");
        auto ms = OpenStore(store.get());
        GraphPlacement p;
        EXPECT_TRUE(ms->GetGraphPlacement("g", &p));
        const uint64_t before = p.unique_id;
        EXPECT_TRUE(ms->HasGraph("g"));
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.DeleteGraphPlacement("g"));
            EXPECT_TRUE(batch.Commit());
        }
        uint64_t uid4 = 0;
        {
            auto txn = store->CreateWriteTxn(false);
            ClusterMetaStore::Batch batch(ms.get(), *txn);
            EXPECT_TRUE(batch.PutGraphPlacement("g", 0, PlacementState::ACTIVE, nullptr, &uid4));
            EXPECT_TRUE(batch.Commit());
        }
        EXPECT_GT(uid4, before);
    }
}
