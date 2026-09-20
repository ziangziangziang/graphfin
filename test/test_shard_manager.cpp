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

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestShardManager : public TuGraphTest {};

namespace {

ShardInfo MakeShard(ShardId id) {
    ShardInfo s;
    s.shard_id = id;
    s.state = ShardState::ONLINE;
    s.name = "shard-" + std::to_string(id);
    s.endpoints = {"127.0.0.1:29092", "127.0.0.1:29093", "127.0.0.1:29094"};
    return s;
}

}  // namespace

TEST_F(TestShardManager, RegistrationValidation) {
    AutoCleanDir cleaner("./test_shard_mgr");
    auto store = std::make_unique<LMDBKvStore>("./test_shard_mgr");
    auto ms = std::make_unique<ClusterMetaStore>();
    {
        auto txn = store->CreateWriteTxn(false);
        ms->Init(store.get(), *txn, true);
        txn->Commit();
    }
    ShardManager mgr(ms.get());
    std::string err;
    {
        auto txn = store->CreateWriteTxn(false);

        ShardInfo bad;  // invalid id
        bad.name = "x";
        bad.endpoints = {"h:1"};
        EXPECT_FALSE(mgr.RegisterShard(*txn, bad, &err));

        ShardInfo no_name = MakeShard(1);
        no_name.name = "";
        EXPECT_FALSE(mgr.RegisterShard(*txn, no_name, &err));

        ShardInfo no_ep = MakeShard(2);
        no_ep.endpoints.clear();
        EXPECT_FALSE(mgr.RegisterShard(*txn, no_ep, &err));

        ShardInfo empty_ep = MakeShard(3);
        empty_ep.endpoints = {"127.0.0.1:1", ""};
        EXPECT_FALSE(mgr.RegisterShard(*txn, empty_ep, &err));

        EXPECT_TRUE(mgr.RegisterShard(*txn, MakeShard(0), &err)) << err;
        txn->Commit();
    }
    EXPECT_EQ(mgr.ShardCount(), 1u);
    ShardInfo got;
    EXPECT_TRUE(mgr.GetShard(0, &got));
    EXPECT_EQ(got.endpoints.size(), 3u);
}

TEST_F(TestShardManager, HealthTracksHeartbeat) {
    AutoCleanDir cleaner("./test_shard_mgr_health");
    auto store = std::make_unique<LMDBKvStore>("./test_shard_mgr_health");
    auto ms = std::make_unique<ClusterMetaStore>();
    {
        auto txn = store->CreateWriteTxn(false);
        ms->Init(store.get(), *txn, true);
        txn->Commit();
    }
    int64_t now = 1000;
    ShardManager mgr(ms.get());
    mgr.SetClock([&now]() { return now; });

    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr.RegisterShard(*txn, MakeShard(0)));
        EXPECT_TRUE(mgr.RegisterShard(*txn, MakeShard(1)));
        txn->Commit();
    }
    // Registration sets the first heartbeat.
    EXPECT_TRUE(mgr.IsHealthy(0, now));
    EXPECT_TRUE(mgr.IsHealthy(0, now + 29999));
    EXPECT_FALSE(mgr.IsHealthy(0, now + 30001));  // beyond the 30s window

    now += 100;
    mgr.Heartbeat(0);
    EXPECT_EQ(mgr.LastHeartbeat(0), now);
    EXPECT_TRUE(mgr.IsHealthy(0, now));
    // Shard 1's heartbeat is still the registration time (1000), which is
    // within the window at now=1100.
    EXPECT_TRUE(mgr.IsHealthy(1, now));
    EXPECT_FALSE(mgr.IsHealthy(1, now + 30000));
}

TEST_F(TestShardManager, PickShardLeastGraphCount) {
    AutoCleanDir cleaner("./test_shard_mgr_pick");
    auto store = std::make_unique<LMDBKvStore>("./test_shard_mgr_pick");
    auto ms = std::make_unique<ClusterMetaStore>();
    {
        auto txn = store->CreateWriteTxn(false);
        ms->Init(store.get(), *txn, true);
        txn->Commit();
    }
    int64_t now = 1000;
    ShardManager mgr(ms.get());
    mgr.SetClock([&now]() { return now; });

    {
        auto txn = store->CreateWriteTxn(false);
        mgr.RegisterShard(*txn, MakeShard(0));
        mgr.RegisterShard(*txn, MakeShard(1));
        // shard 0 gets two graphs, shard 1 none.
        ms->PutGraphPlacement(*txn, "a0", 0, PlacementState::ACTIVE);
        ms->PutGraphPlacement(*txn, "a1", 0, PlacementState::ACTIVE);
        txn->Commit();
    }
    EXPECT_EQ(ms->GraphCountOnShard(0), 2u);
    EXPECT_EQ(ms->GraphCountOnShard(1), 0u);

    ShardId pick = INVALID_SHARD_ID;
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 1u);  // least loaded

    // Balance: shard 0 has 2, shard 1 has 1 -> still 1.
    {
        auto txn = store->CreateWriteTxn(false);
        ms->PutGraphPlacement(*txn, "b0", 1, PlacementState::ACTIVE);
        txn->Commit();
    }
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 1u);

    // Equalize: 2 vs 2 -> tie-break to the lowest id (0).
    {
        auto txn = store->CreateWriteTxn(false);
        ms->PutGraphPlacement(*txn, "b1", 1, PlacementState::ACTIVE);
        txn->Commit();
    }
    EXPECT_EQ(ms->GraphCountOnShard(0), 2u);
    EXPECT_EQ(ms->GraphCountOnShard(1), 2u);
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 0u);

    // Stale shards are not eligible.
    now += 40000;
    EXPECT_FALSE(mgr.PickShard(now, &pick));
    mgr.Heartbeat(1);
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 1u);  // only shard 1 is healthy now

    // DRAINING excluded.
    mgr.Heartbeat(0);
    {
        auto txn = store->CreateWriteTxn(false);
        mgr.SetShardState(*txn, 1, ShardState::DRAINING);
        txn->Commit();
    }
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 0u);
}

TEST_F(TestShardManager, PickShardRoundRobin) {
    AutoCleanDir cleaner("./test_shard_mgr_rr");
    auto store = std::make_unique<LMDBKvStore>("./test_shard_mgr_rr");
    auto ms = std::make_unique<ClusterMetaStore>();
    {
        auto txn = store->CreateWriteTxn(false);
        ms->Init(store.get(), *txn, true);
        txn->Commit();
    }
    int64_t now = 5000;
    ShardManager mgr(ms.get(), ShardManager::Config{30000,
                                                    ShardManager::PlacementStrategy::ROUND_ROBIN});
    mgr.SetClock([&now]() { return now; });
    {
        auto txn = store->CreateWriteTxn(false);
        mgr.RegisterShard(*txn, MakeShard(0));
        mgr.RegisterShard(*txn, MakeShard(1));
        mgr.RegisterShard(*txn, MakeShard(2));
        txn->Commit();
    }
    ShardId pick = INVALID_SHARD_ID;
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 0u);
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 1u);
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 2u);
    EXPECT_TRUE(mgr.PickShard(now, &pick));
    EXPECT_EQ(pick, 0u);  // wraps
}

TEST_F(TestShardManager, DeregisterRefusedWhileGraphsRemain) {
    AutoCleanDir cleaner("./test_shard_mgr_dereg");
    auto store = std::make_unique<LMDBKvStore>("./test_shard_mgr_dereg");
    auto ms = std::make_unique<ClusterMetaStore>();
    {
        auto txn = store->CreateWriteTxn(false);
        ms->Init(store.get(), *txn, true);
        txn->Commit();
    }
    ShardManager mgr(ms.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr.RegisterShard(*txn, MakeShard(0));
        ms->PutGraphPlacement(*txn, "g0", 0, PlacementState::ACTIVE);
        txn->Commit();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_FALSE(mgr.DeregisterShard(*txn, 0));  // graph still there
        txn->Commit();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        ms->DeleteGraphPlacement(*txn, "g0");
        txn->Commit();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr.DeregisterShard(*txn, 0));
        txn->Commit();
    }
    EXPECT_EQ(mgr.ShardCount(), 0u);
    EXPECT_EQ(mgr.LastHeartbeat(0), -1);
}
