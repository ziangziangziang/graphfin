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

#include <algorithm>
#include <memory>
#include <string>

#include "gtest/gtest.h"

#include "core/kv_store.h"
#include "cluster/cluster_meta_store.h"
#include "cluster/shard_manager.h"
#include "cluster/router.h"
#include "cluster/cluster_control.h"

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestClusterControl : public TuGraphTest {};

namespace {

ShardInfo MakeShard(ShardId id, const std::string& ep = "127.0.0.1:29092") {
    ShardInfo s;
    s.shard_id = id;
    s.state = ShardState::ONLINE;
    s.name = "shard-" + std::to_string(id);
    s.endpoints = {ep};
    return s;
}

class MockLocator : public ShardLocator {
 public:
    std::string LocateLeader(ShardId) override { return endpoint; }
    std::string endpoint = "leader:1";
};

struct Fixture {
    std::unique_ptr<LMDBKvStore> store;
    std::unique_ptr<ClusterMetaStore> ms;
    std::unique_ptr<ShardManager> mgr;
    std::unique_ptr<MockLocator> locator;
    std::unique_ptr<Router> router;
    std::unique_ptr<ClusterControl> control;
    int64_t now = 1000;

    void Init(const std::string& dir) {
        store.reset(new LMDBKvStore(dir));
        ms.reset(new ClusterMetaStore());
        {
            auto txn = store->CreateWriteTxn(false);
            ms->Init(store.get(), *txn, true);
            txn->Commit();
        }
        mgr.reset(new ShardManager(ms.get()));
        mgr->SetClock([this]() { return now; });
        locator.reset(new MockLocator());
        router.reset(new Router(ms.get(), mgr.get(), locator.get()));
        control.reset(new ClusterControl(ms.get(), mgr.get(), router.get()));
    }
};

}  // namespace

TEST_F(TestClusterControl, CreatePlaceResolveFence) {
    AutoCleanDir cleaner("./test_cluster_control");
    Fixture f;
    f.Init("./test_cluster_control");

    // A graph cannot be created without a healthy shard.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g0", f.now), ControlStatus::NO_HEALTHY_SHARD);
        txn->Commit();
    }

    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(1)), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_EQ(f.control->ListShards().size(), 2u);

    // Create places the graph and returns its placement version + unique id.
    PlacementVersion pv = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now, &pv, &uid), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(pv, 0u);
    EXPECT_GT(uid, 0u);

    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.placement_version, pv);
    EXPECT_EQ(p.unique_id, uid);

    // Duplicate creation is rejected.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now), ControlStatus::GRAPH_EXISTS);
        txn->Abort();
        f.ms->RollbackStaged();
    }

    // Resolve routes by logical name.
    RouteTarget t;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, p.shard_id);
    EXPECT_FALSE(t.endpoint.empty());

    // Receiver-side fencing: a stale sender is rejected.
    PlacementVersion cur = 0;
    EXPECT_TRUE(f.control->Fence("g1", pv, &cur));
    EXPECT_EQ(cur, pv);
    EXPECT_FALSE(f.control->Fence("g1", pv > 0 ? pv - 1 : 0));
    EXPECT_FALSE(f.control->Fence("nonexistent", 1));

    // Move the graph (new version) and confirm the old sender is now stale.
    PlacementVersion pv2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(0)), ControlStatus::OK);
        f.ms->PutGraphPlacement(*txn, "g1", (p.shard_id == 0) ? 1 : 0, PlacementState::ACTIVE,
                                &pv2);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(pv2, pv);
    EXPECT_FALSE(f.control->Fence("g1", pv));   // old version now stale
    EXPECT_TRUE(f.control->Fence("g1", pv2));    // current version accepted
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.unique_id, uid);  // immutable identity preserved across a move

    // Delete then re-create.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->DeleteGraph(*txn, "g1"), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::GRAPH_NOT_FOUND);
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
}

// T4.2 — forwarded-write fencing end to end (in the test harness the caller
// models "the router", the destination models "the receiving shard").
TEST_F(TestClusterControl, ForwardedWriteIsFencedByTheReceiver) {
    AutoCleanDir cleaner("./test_cluster_control_fence");
    Fixture f;
    f.Init("./test_cluster_control_fence");

    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(1)), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }

    // Router resolves g1 (as routed, on shard S with version V).
    PlacementVersion v1 = 0;
    ShardId s = INVALID_SHARD_ID;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now, &v1), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    RouteTarget routed;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &routed), RouteStatus::OK);
    s = routed.shard_id;
    const ShardId other = (s == 0) ? 1 : 0;

    // Deliver the routed request at S with the version it was routed with.
    EXPECT_TRUE(f.control->FenceAt(s, "g1", routed.version));

    // Move the graph to the other shard (new version).
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        f.ms->PutGraphPlacement(*txn, "g1", other, PlacementState::ACTIVE, &v2);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(v2, v1);

    // Replaying the old request at the NEW destination is rejected.
    EXPECT_FALSE(f.control->FenceAt(other, "g1", routed.version));
    // And at the OLD destination too — it no longer hosts the graph.
    PlacementVersion cur = 0;
    EXPECT_FALSE(f.control->FenceAt(s, "g1", routed.version, &cur));
    EXPECT_EQ(cur, v2);

    // A fresh resolve succeeds and its version is accepted on the new shard.
    RouteTarget t2;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t2), RouteStatus::OK);
    EXPECT_EQ(t2.shard_id, other);
    EXPECT_TRUE(f.control->FenceAt(other, "g1", t2.version));
}

// T4.3 — admin listing of what lives where.
TEST_F(TestClusterControl, ListGraphsOnShard) {
    AutoCleanDir cleaner("./test_cluster_control_list");
    Fixture f;
    f.Init("./test_cluster_control_list");
    // Stage-publish: shards must be registered AND committed before creation
    // can place onto them, because placement reads committed counts only.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(1)), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "b", f.now), ControlStatus::OK);
        EXPECT_EQ(f.control->CreateGraph(*txn, "a", f.now), ControlStatus::OK);
        EXPECT_EQ(f.control->CreateGraph(*txn, "c", f.now), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    // All three were created in one batch, so each saw zero committed graphs and
    // picked shard 0 (ties break to the lowest id). Counts only move on commit.
    auto all = f.control->ListAllGraphs();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], "a");
    EXPECT_EQ(all[1], "b");
    EXPECT_EQ(all[2], "c");
    auto s0 = f.control->ListGraphsOnShard(0);
    auto s1 = f.control->ListGraphsOnShard(1);
    EXPECT_EQ(s0.size() + s1.size(), 3u);
    EXPECT_TRUE(std::is_sorted(s0.begin(), s0.end()));
    EXPECT_TRUE(std::is_sorted(s1.begin(), s1.end()));
    EXPECT_TRUE(f.control->ListGraphsOnShard(7).empty());
}

// Move lifecycle: begin (quiesce) -> complete (atomic cutover) or abort.
TEST_F(TestClusterControl, MoveLifecycle) {
    AutoCleanDir cleaner("./test_cluster_control_move");
    Fixture f;
    f.Init("./test_cluster_control_move");
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(*txn, MakeShard(1)), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    PlacementVersion v0 = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now, &v0, &uid), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    const ShardId src = p.shard_id;
    const ShardId dst = (src == 0) ? 1 : 0;

    // Guards.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->BeginMove(*txn, "nope", dst, f.now), ControlStatus::GRAPH_NOT_FOUND);
        EXPECT_EQ(f.control->BeginMove(*txn, "g1", src, f.now), ControlStatus::PERSIST_FAILED);
        EXPECT_EQ(f.control->BeginMove(*txn, "g1", 7, f.now), ControlStatus::NO_HEALTHY_SHARD);
        txn->Abort();
        f.ms->RollbackStaged();
    }

    // Begin: graph quiesces on its current shard (not routable while moving).
    PlacementVersion v1 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->BeginMove(*txn, "g1", dst, f.now, &v1), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(v1, v0);
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, src);
    EXPECT_EQ(p.State(), PlacementState::MOVING);
    EXPECT_EQ(p.unique_id, uid);
    RouteTarget t;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::PLACEMENT_NOT_ACTIVE);
    // The pre-move version no longer routes.
    EXPECT_EQ(f.control->Fence("g1", v0), false);

    // Abort: back to ACTIVE on the source, identity preserved.
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->AbortMove(*txn, "g1", &v2), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(v2, v1);
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, src);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.unique_id, uid);

    // Complete the move for real this time.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->BeginMove(*txn, "g1", dst, f.now), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    PlacementVersion v3 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CompleteMove(*txn, "g1", dst, &v3), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, dst);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.unique_id, uid);
    // Old versions are stale everywhere; the new version routes to dst.
    EXPECT_FALSE(f.control->FenceAt(dst, "g1", v1));
    EXPECT_TRUE(f.control->FenceAt(dst, "g1", v3));
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, dst);

    // Completing/aborting a non-moving graph fails.
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CompleteMove(*txn, "g1", dst), ControlStatus::PERSIST_FAILED);
        EXPECT_EQ(f.control->AbortMove(*txn, "g1"), ControlStatus::PERSIST_FAILED);
        txn->Abort();
        f.ms->RollbackStaged();
    }
}
