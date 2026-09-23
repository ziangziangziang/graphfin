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
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g0", f.now), ControlStatus::NO_HEALTHY_SHARD);
        batch.Abort();
    }

    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.control->ListShards().size(), 2u);

    // Create places the graph and returns its placement version + unique id.
    PlacementVersion pv = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g1", f.now, &pv, &uid), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
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
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g1", f.now), ControlStatus::GRAPH_EXISTS);
        batch.Abort();
    }

    // Resolve routes by logical name.
    RouteTarget t;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, p.shard_id);
    EXPECT_FALSE(t.endpoint.empty());

    // Receiver-side fencing: a stale sender is rejected.
    PlacementVersion cur = 0;
    EXPECT_TRUE(f.control->Fence("g1", uid, pv, &cur, nullptr));
    EXPECT_EQ(cur, pv);
    EXPECT_FALSE(f.control->Fence("g1", uid, pv > 0 ? pv - 1 : 0));
    EXPECT_FALSE(f.control->Fence("nonexistent", 0, 1));

    // Move the graph (new version) and confirm the old sender is now stale.
    PlacementVersion pv2 = 0;
    uint64_t uid2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", (p.shard_id == 0) ? 1 : 0, PlacementState::ACTIVE,
                                &pv2, &uid2));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(pv2, pv);
    EXPECT_EQ(uid2, uid);  // move preserves the immutable identity
    EXPECT_FALSE(f.control->Fence("g1", uid, pv));   // old version now stale
    EXPECT_TRUE(f.control->Fence("g1", uid, pv2, nullptr, nullptr));    // current version accepted
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.unique_id, uid);  // immutable identity preserved across a move

    // Delete then re-create.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->DeleteGraph(batch, "g1"), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::GRAPH_NOT_FOUND);
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g1", f.now), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
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
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }

    // Router resolves g1 (as routed, on shard S with version V).
    PlacementVersion v1 = 0;
    uint64_t uid1 = 0;
    ShardId s = INVALID_SHARD_ID;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g1", f.now, &v1, &uid1), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget routed;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &routed), RouteStatus::OK);
    s = routed.shard_id;
    const ShardId other = (s == 0) ? 1 : 0;

    // Deliver the routed request at S with the version it was routed with.
    EXPECT_TRUE(f.control->FenceAt(s, "g1", routed.unique_id, routed.version));

    // Move the graph to the other shard (new version).
    PlacementVersion v2 = 0;
    uint64_t uid2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", other, PlacementState::ACTIVE, &v2, &uid2));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(v2, v1);
    EXPECT_EQ(uid2, uid1);  // move preserves the immutable identity

    // Replaying the old request at the NEW destination is rejected.
    EXPECT_FALSE(f.control->FenceAt(other, "g1", routed.unique_id, routed.version));
    // And at the OLD destination too — it no longer hosts the graph.
    PlacementVersion cur = 0;
    EXPECT_FALSE(f.control->FenceAt(s, "g1", routed.unique_id, routed.version, &cur, nullptr));
    EXPECT_EQ(cur, v2);

    // A fresh resolve succeeds and its version is accepted on the new shard.
    RouteTarget t2;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t2), RouteStatus::OK);
    EXPECT_EQ(t2.shard_id, other);
    EXPECT_TRUE(f.control->FenceAt(other, "g1", t2.unique_id, t2.version));
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
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "b", f.now), ControlStatus::OK);
        EXPECT_EQ(f.control->CreateGraph(batch, "a", f.now), ControlStatus::OK);
        EXPECT_EQ(f.control->CreateGraph(batch, "c", f.now), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
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

// R8 (partial): batch sees its own writes for validation. Duplicate creates in
// one batch must not both succeed; create-then-delete in one batch is allowed
// and converges to absent.
TEST_F(TestClusterControl, BatchSeesOwnWritesForValidation) {
    AutoCleanDir cleaner("./test_cluster_control_r8");
    Fixture f;
    f.Init("./test_cluster_control_r8");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "dup", f.now), ControlStatus::OK);
        // Second create of the same name in the SAME batch must be rejected.
        EXPECT_EQ(f.control->CreateGraph(batch, "dup", f.now), ControlStatus::GRAPH_EXISTS);
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("dup", &p), ControlStatus::OK);
    // Create + delete in one batch converges to absent.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "tmp", f.now), ControlStatus::OK);
        EXPECT_EQ(f.control->DeleteGraph(batch, "tmp"), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.control->GetPlacement("tmp", &p), ControlStatus::GRAPH_NOT_FOUND);
}

// Move lifecycle: begin (quiesce) -> complete (atomic cutover) or abort.
TEST_F(TestClusterControl, MoveLifecycle) {
    AutoCleanDir cleaner("./test_cluster_control_move");
    Fixture f;
    f.Init("./test_cluster_control_move");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    PlacementVersion v0 = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g1", f.now, &v0, &uid), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    const ShardId src = p.shard_id;
    const ShardId dst = (src == 0) ? 1 : 0;

    // Guards.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "nope", dst, f.now), ControlStatus::GRAPH_NOT_FOUND);
        EXPECT_EQ(f.control->BeginMove(batch, "g1", src, f.now), ControlStatus::PERSIST_FAILED);
        EXPECT_EQ(f.control->BeginMove(batch, "g1", 7, f.now), ControlStatus::NO_HEALTHY_SHARD);
        batch.Abort();
    }

    // Begin: graph quiesces on its current shard (not routable while moving).
    PlacementVersion v1 = 0;
    uint64_t uid1 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "g1", dst, f.now, &v1, &uid1), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(v1, v0);
    EXPECT_EQ(uid1, uid);  // Begin reports the graph's immutable identity
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, src);
    EXPECT_EQ(p.State(), PlacementState::MOVING);
    EXPECT_EQ(p.unique_id, uid);
    RouteTarget t;
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::PLACEMENT_NOT_ACTIVE);
    // The pre-move version no longer routes.
    EXPECT_EQ(f.control->Fence("g1", uid, v0), false);

    // Abort: back to ACTIVE on the source, identity preserved.
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->AbortMove(batch, "g1", uid1, v1, &v2), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(v2, v1);
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, src);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.unique_id, uid);

    // Complete the move for real this time.
    PlacementVersion v12 = 0;
    uint64_t uid12 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "g1", dst, f.now, &v12, &uid12),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(uid12, uid);
    PlacementVersion v3 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CompleteMove(batch, "g1", dst, uid12, v12, &v3),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, dst);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.unique_id, uid);
    // Old versions are stale everywhere; the new version routes to dst.
    EXPECT_FALSE(f.control->FenceAt(dst, "g1", uid, v1));
    EXPECT_TRUE(f.control->FenceAt(dst, "g1", uid, v3));
    EXPECT_EQ(f.control->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, dst);

    // R7: completing/aborting an already-ACTIVE graph is an idempotent OK
    // (replay of an applied attempt), not PERSIST_FAILED. Complete binds to
    // the attempt tuple, but ACTIVE-on-dst with the same UID short-circuits
    // to OK regardless of the expected version; Abort on ACTIVE with the same
    // UID is a no-mutation OK reporting the current version.
    {
        GraphPlacement cur;
        EXPECT_EQ(f.control->GetPlacement("g1", &cur), ControlStatus::OK);
        EXPECT_EQ(cur.unique_id, uid);
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        PlacementVersion out = 0;
        EXPECT_EQ(f.control->CompleteMove(batch, "g1", dst, cur.unique_id, v1, &out),
                  ControlStatus::OK);
        EXPECT_EQ(out, cur.placement_version);
        EXPECT_EQ(f.control->AbortMove(batch, "g1", cur.unique_id, cur.placement_version,
                                       &out),
                  ControlStatus::OK);
        EXPECT_EQ(out, cur.placement_version);
        batch.Abort();
    }
}

// R3: delete/recreate retires the old incarnation. The old (uid, version)
// tuple is rejected everywhere; the route carries the fresh UID.
TEST_F(TestClusterControl, RecreateRetiresOldIncarnation) {
    AutoCleanDir cleaner("./test_cluster_control_r3");
    Fixture f;
    f.Init("./test_cluster_control_r3");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    PlacementVersion v1 = 0;
    uint64_t uid1 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g", f.now, &v1, &uid1), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t1;
    EXPECT_EQ(f.control->Resolve("g", f.now, &t1), RouteStatus::OK);
    EXPECT_EQ(t1.unique_id, uid1);
    EXPECT_TRUE(f.control->FenceAt(t1.shard_id, "g", uid1, v1));

    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->DeleteGraph(batch, "g"), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    PlacementVersion v2 = 0;
    uint64_t uid2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g", f.now, &v2, &uid2), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_NE(uid2, uid1);
    // Delayed write bearing the old incarnation is rejected on every shard,
    // and reports the authoritative tuple for retry.
    PlacementVersion cur_v = 0;
    uint64_t cur_uid = 0;
    EXPECT_FALSE(f.control->FenceAt(t1.shard_id, "g", uid1, v1, &cur_v, &cur_uid));
    EXPECT_EQ(cur_uid, uid2);
    EXPECT_EQ(cur_v, v2);
    EXPECT_FALSE(f.control->Fence("g", uid1, v1));
    // Fresh tuple routes and fences.
    RouteTarget t2;
    EXPECT_EQ(f.control->Resolve("g", f.now, &t2), RouteStatus::OK);
    EXPECT_EQ(t2.unique_id, uid2);
    EXPECT_TRUE(f.control->FenceAt(t2.shard_id, "g", uid2, v2));
    // Same-batch delete + recreate also retires the UID.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->DeleteGraph(batch, "g"), ControlStatus::OK);
        PlacementVersion v3 = 0;
        uint64_t uid3 = 0;
        EXPECT_EQ(f.control->CreateGraph(batch, "g", f.now, &v3, &uid3), ControlStatus::OK);
        EXPECT_NE(uid3, uid2);
        EXPECT_TRUE(batch.Commit());
        EXPECT_FALSE(f.control->Fence("g", uid2, v2));
        EXPECT_TRUE(f.control->Fence("g", uid3, v3));
    }
}

// R4: the ownership guard rejects stale epochs, future epochs (catalog lag
// must refresh, never jump ahead), wrong shards, and non-ACTIVE placements.
TEST_F(TestClusterControl, FenceRejectsStaleFutureWrongShardAndNonActive) {
    AutoCleanDir cleaner("./test_cluster_control_r4");
    Fixture f;
    f.Init("./test_cluster_control_r4");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    PlacementVersion v0 = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g", f.now, &v0, &uid), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g", &p), ControlStatus::OK);
    const ShardId src = p.shard_id;
    const ShardId other = (src == 0) ? 1 : 0;

    // Future epoch: a sender racing ahead of the authoritative version fails
    // closed (and learns the authoritative tuple).
    PlacementVersion cur_v = 0;
    uint64_t cur_uid = 0;
    EXPECT_FALSE(f.control->Fence("g", uid, v0 + 100, &cur_v, &cur_uid));
    EXPECT_EQ(cur_v, v0);
    EXPECT_EQ(cur_uid, uid);
    // Wrong UID and wrong shard.
    EXPECT_FALSE(f.control->Fence("g", uid + 1, v0));
    EXPECT_FALSE(f.control->FenceAt(other, "g", uid, v0, &cur_v, &cur_uid));
    EXPECT_EQ(cur_v, v0);
    // Non-ACTIVE placements never fence, even with the current version.
    PlacementVersion vm = 0;
    uint64_t uidm = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "g", other, f.now, &vm, &uidm),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(uidm, uid);
    EXPECT_FALSE(f.control->Fence("g", uid, vm));
    EXPECT_FALSE(f.control->FenceAt(src, "g", uid, vm));
    RouteTarget t;
    EXPECT_EQ(f.control->Resolve("g", f.now, &t), RouteStatus::PLACEMENT_NOT_ACTIVE);
    // Cutover races a queued write bearing the pre-move tuple: rejected.
    PlacementVersion vc = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CompleteMove(batch, "g", other, uid, vm, &vc),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_FALSE(f.control->FenceAt(other, "g", uid, v0));
    EXPECT_FALSE(f.control->FenceAt(src, "g", uid, vm));
    EXPECT_TRUE(f.control->FenceAt(other, "g", uid, vc));
}

// R7: move completion/abort bind to the attempt tuple. A delayed completion
// from an earlier attempt cannot cut over a later one, and a stale abort
// cannot cancel a newer move. Applied attempts replay idempotently.
TEST_F(TestClusterControl, MoveCompletionIsAttemptBound) {
    AutoCleanDir cleaner("./test_cluster_control_r7");
    Fixture f;
    f.Init("./test_cluster_control_r7");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(0)), ControlStatus::OK);
        EXPECT_EQ(f.control->RegisterShard(batch, MakeShard(1)), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    PlacementVersion v0 = 0;
    uint64_t uid = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CreateGraph(batch, "g", f.now, &v0, &uid), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g", &p), ControlStatus::OK);
    const ShardId src = p.shard_id;
    const ShardId dst = (src == 0) ? 1 : 0;

    // Attempt 1: Begin (MOVING v1), then abort (ACTIVE v2).
    PlacementVersion v1 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "g", dst, f.now, &v1, nullptr),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    // Wrong-version / wrong-UID completions and aborts are stale, not applied.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        PlacementVersion out = 0;
        EXPECT_EQ(f.control->CompleteMove(batch, "g", dst, uid, v1 + 100, &out),
                  ControlStatus::STALE_PLACEMENT);
        EXPECT_EQ(f.control->CompleteMove(batch, "g", dst, uid + 1, v1, &out),
                  ControlStatus::STALE_PLACEMENT);
        EXPECT_EQ(f.control->AbortMove(batch, "g", uid, v1 + 100, &out),
                  ControlStatus::STALE_PLACEMENT);
        batch.Abort();
    }
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->AbortMove(batch, "g", uid, v1, &v2), ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    // Attempt 1's MOVING version is now stale: completing with it is rejected.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        PlacementVersion out = 0;
        EXPECT_EQ(f.control->CompleteMove(batch, "g", dst, uid, v1, &out),
                  ControlStatus::STALE_PLACEMENT);
        batch.Abort();
    }
    // Attempt 2: Begin (MOVING v3). Attempt 1's abort must not cancel it.
    PlacementVersion v3 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->BeginMove(batch, "g", dst, f.now, &v3, nullptr),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        PlacementVersion out = 0;
        EXPECT_EQ(f.control->AbortMove(batch, "g", uid, v1, &out), ControlStatus::STALE_PLACEMENT);
        batch.Abort();
    }
    // Real completion binds to attempt 2; replay is idempotent.
    PlacementVersion v4 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_EQ(f.control->CompleteMove(batch, "g", dst, uid, v3, &v4),
                  ControlStatus::OK);
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        PlacementVersion out = 0;
        EXPECT_EQ(f.control->CompleteMove(batch, "g", dst, uid, v3, &out),
                  ControlStatus::OK);
        EXPECT_EQ(out, v4);
        batch.Abort();
    }
    EXPECT_EQ(f.control->GetPlacement("g", &p), ControlStatus::OK);
    EXPECT_EQ(p.shard_id, dst);
    EXPECT_EQ(p.unique_id, uid);
}
