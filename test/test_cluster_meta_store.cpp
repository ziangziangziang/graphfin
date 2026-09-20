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

#include <cstdio>
#include <memory>
#include <string>

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
            ms->CommitStaged();
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
            EXPECT_TRUE(ms->RegisterShard(*txn, MakeShard(0)));
            EXPECT_TRUE(ms->RegisterShard(*txn, MakeShard(1)));
            txn->Commit();
            ms->CommitStaged();
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
            EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g1", 0, PlacementState::ACTIVE, &pv));
            EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g2", 1, PlacementState::ACTIVE));
            txn->Commit();
            ms->CommitStaged();
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
            PlacementVersion pv2 = 0;
            EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g1", 1, PlacementState::MOVING, &pv2));
            txn->Commit();
            ms->CommitStaged();
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
            EXPECT_FALSE(ms->RemoveShard(*txn, 1));
            txn->Commit();
            ms->CommitStaged();
        }
        // Delete g2; g1 is still on shard 1, so removal is still refused.
        {
            auto txn = store->CreateWriteTxn(false);
            EXPECT_TRUE(ms->DeleteGraphPlacement(*txn, "g2"));
            txn->Commit();
            ms->CommitStaged();
        }
        EXPECT_FALSE(ms->HasGraph("g2"));
        EXPECT_EQ(ms->GraphCount(), 1u);
        {
            auto txn = store->CreateWriteTxn(false);
            EXPECT_FALSE(ms->RemoveShard(*txn, 1));
            txn->Commit();
            ms->CommitStaged();
        }
        // Move g1 back to shard 0; now shard 1 is empty and removable.
        {
            auto txn = store->CreateWriteTxn(false);
            EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g1", 0, PlacementState::ACTIVE));
            txn->Commit();
            ms->CommitStaged();
        }
        {
            auto txn = store->CreateWriteTxn(false);
            EXPECT_TRUE(ms->RemoveShard(*txn, 1));
            txn->Commit();
            ms->CommitStaged();
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
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g3", 0, PlacementState::ACTIVE));
        txn->Commit();
            ms->CommitStaged();
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
    auto txn = store->CreateWriteTxn(false);
    EXPECT_TRUE(ms->RegisterShard(*txn, MakeShard(0)));
    txn->Commit();
            ms->CommitStaged();
    EXPECT_GT(ms->Version(), v);
    v = ms->Version();

    txn = store->CreateWriteTxn(false);
    EXPECT_TRUE(ms->PutGraphPlacement(*txn, "a", 0, PlacementState::ACTIVE));
    txn->Commit();
            ms->CommitStaged();
    EXPECT_GT(ms->Version(), v);

    // Explicit set is honoured and persists.
    txn = store->CreateWriteTxn(false);
    EXPECT_TRUE(ms->SetVersion(*txn, 12345));
    txn->Commit();
            ms->CommitStaged();
    EXPECT_EQ(ms->Version(), 12345u);
}

TEST_F(TestClusterMetaStore, StagingIsolationAndRollback) {
    AutoCleanDir cleaner("./test_cluster_meta_stage");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_stage");
    auto ms = OpenStore(store.get());

    // A staged, uncommitted placement must NOT be visible to readers.
    {
        auto txn = store->CreateWriteTxn(false);
        PlacementVersion pv = 0;
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g1", 0, PlacementState::ACTIVE, &pv));
        EXPECT_TRUE(ms->HasStaged());
        EXPECT_EQ(ms->StagedCount(), 1u);
        GraphPlacement p;
        GraphId id = 0;
        EXPECT_FALSE(ms->GetGraphPlacement("g1", &p, &id));  // not published
        EXPECT_FALSE(ms->HasGraph("g1"));
        EXPECT_EQ(ms->GraphCount(), 0u);
        // Abort and roll back: no divergence between memory and storage.
        txn->Abort();
        ms->RollbackStaged();
        EXPECT_FALSE(ms->HasStaged());
    }
    EXPECT_FALSE(ms->HasGraph("g1"));
    EXPECT_EQ(ms->GraphCount(), 0u);

    // Commit + publish makes the placement visible.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g1", 0, PlacementState::ACTIVE));
        txn->Commit();
        ms->CommitStaged();
    }
    GraphPlacement p;
    GraphId id = 0;
    EXPECT_TRUE(ms->GetGraphPlacement("g1", &p, &id));
    EXPECT_EQ(ms->GraphCount(), 1u);

    // A batch publishes atomically: neither graph is visible until CommitStaged.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g2", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, "g3", 0, PlacementState::ACTIVE));
        EXPECT_EQ(ms->StagedCount(), 2u);
        EXPECT_FALSE(ms->HasGraph("g2"));
        EXPECT_FALSE(ms->HasGraph("g3"));
        EXPECT_EQ(ms->GraphCount(), 1u);
        txn->Commit();
        ms->CommitStaged();
    }
    EXPECT_TRUE(ms->HasGraph("g2"));
    EXPECT_TRUE(ms->HasGraph("g3"));
    EXPECT_EQ(ms->GraphCount(), 3u);
    EXPECT_FALSE(ms->HasStaged());
}

TEST_F(TestClusterMetaStore, MemoryFootprintIsCompact) {
    AutoCleanDir cleaner("./test_cluster_meta_mem");
    auto store = std::make_unique<LMDBKvStore>("./test_cluster_meta_mem");
    auto ms = OpenStore(store.get());

    const size_t N = 20000;
    auto txn = store->CreateWriteTxn(false);
    for (size_t i = 0; i < N; i++) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "graph_%06zu", i);
        EXPECT_TRUE(ms->PutGraphPlacement(*txn, buf, static_cast<ShardId>(i % 8),
                                          PlacementState::ACTIVE));
    }
    txn->Commit();
            ms->CommitStaged();

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
