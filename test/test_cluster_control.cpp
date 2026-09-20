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

    // Create places the graph and returns its placement version.
    PlacementVersion pv = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        EXPECT_EQ(f.control->CreateGraph(*txn, "g1", f.now, &pv), ControlStatus::OK);
        txn->Commit();
        f.ms->CommitStaged();
    }
    EXPECT_GT(pv, 0u);

    GraphPlacement p;
    EXPECT_EQ(f.control->GetPlacement("g1", &p), ControlStatus::OK);
    EXPECT_EQ(p.State(), PlacementState::ACTIVE);
    EXPECT_EQ(p.placement_version, pv);

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
