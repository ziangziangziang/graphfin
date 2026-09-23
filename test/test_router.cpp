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

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestRouter : public TuGraphTest {};

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
    std::string LocateLeader(ShardId shard) override {
        calls++;
        last_shard = shard;
        return endpoint;
    }
    int calls = 0;
    ShardId last_shard = INVALID_SHARD_ID;
    std::string endpoint = "leader:1234";
};

struct Fixture {
    std::unique_ptr<LMDBKvStore> store;
    std::unique_ptr<ClusterMetaStore> ms;
    std::unique_ptr<ShardManager> mgr;
    std::unique_ptr<Router> router;
    std::unique_ptr<MockLocator> locator;
    int64_t now = 1000;

    void Init(const std::string& dir, Router::Config rcfg = Router::Config{}) {
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
        router.reset(new Router(ms.get(), mgr.get(), locator.get(), rcfg));
    }
};

}  // namespace

TEST_F(TestRouter, ResolveUnknownAndActive) {
    AutoCleanDir cleaner("./test_router_basic");
    Fixture f;
    f.Init("./test_router_basic");

    RouteTarget t;
    EXPECT_EQ(f.router->Resolve("nope", f.now, &t), RouteStatus::GRAPH_NOT_FOUND);

    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, 0u);
    EXPECT_EQ(t.endpoint, "leader:1234");
    EXPECT_EQ(f.locator->calls, 1);

    // Not-yet-active placement is not routable.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g2", 0, PlacementState::CREATING));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.router->Resolve("g2", f.now, &t), RouteStatus::PLACEMENT_NOT_ACTIVE);
}

TEST_F(TestRouter, CacheHitAndTtl) {
    AutoCleanDir cleaner("./test_router_cache");
    Fixture f;
    f.Init("./test_router_cache", Router::Config{5000, 65536});
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(f.router->Resolve("g1", f.now + 100, &t), RouteStatus::OK);
    EXPECT_EQ(f.locator->calls, 1);  // cached, no second locate
    EXPECT_EQ(f.router->CacheSize(), 1u);

    // Beyond TTL the route is revalidated (locator called again).
    EXPECT_EQ(f.router->Resolve("g1", f.now + 6000, &t), RouteStatus::OK);
    EXPECT_EQ(f.locator->calls, 2);
}

TEST_F(TestRouter, ValidateDetectsStalePlacement) {
    AutoCleanDir cleaner("./test_router_stale");
    Fixture f;
    f.Init("./test_router_stale");
    PlacementVersion v0 = 0;
    uint64_t uid0 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(1)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &v0, &uid0));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    // Caller at the current version is fine.
    EXPECT_EQ(f.router->Validate("g1", uid0, v0, f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, 0u);

    // Move the graph to shard 1; a stale caller is rejected but told where to go.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.router->Validate("g1", uid0, v0, f.now, &t), RouteStatus::STALE_PLACEMENT);
    EXPECT_EQ(t.shard_id, 1u);

    // And a fresh resolve follows the move.
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.shard_id, 1u);
    EXPECT_GT(t.version, v0);
}

TEST_F(TestRouter, UnhealthyShardNotRouted) {
    AutoCleanDir cleaner("./test_router_health");
    Fixture f;
    f.Init("./test_router_health");
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);

    // Advance past the shard's heartbeat window: no longer healthy.
    EXPECT_EQ(f.router->Resolve("g1", f.now + 40000, &t), RouteStatus::NO_HEALTHY_SHARD);
}

TEST_F(TestRouter, EndpointFallbackWhenLocatorEmpty) {
    AutoCleanDir cleaner("./test_router_fallback");
    Fixture f;
    f.Init("./test_router_fallback");
    f.locator->endpoint = "";  // locator cannot resolve a leader
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(3, "127.0.0.1:29093")));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 3, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.endpoint, "127.0.0.1:29093");  // fell back to the registered endpoint
}

TEST_F(TestRouter, CacheIsBounded) {
    AutoCleanDir cleaner("./test_router_bound");
    Fixture f;
    f.Init("./test_router_bound", Router::Config{5000, 2});  // cap = 2
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        for (int i = 0; i < 5; i++) {
            EXPECT_TRUE(batch.PutGraphPlacement(
                "g" + std::to_string(i), 0, PlacementState::ACTIVE));
        }
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(f.router->Resolve("g" + std::to_string(i), f.now, &t), RouteStatus::OK);
        EXPECT_LE(f.router->CacheSize(), 2u);
    }
}

// R3: a warmed route is bound to the incarnation. Recreating the graph
// retires the cached UID: resolve returns the fresh UID and the old tuple
// validates stale.
TEST_F(TestRouter, CacheInvalidatedByRecreate) {
    AutoCleanDir cleaner("./test_router_recreate");
    Fixture f;
    f.Init("./test_router_recreate");
    PlacementVersion v0 = 0;
    uint64_t uid0 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0)));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &v0, &uid0));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t0;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t0), RouteStatus::OK);
    EXPECT_EQ(t0.unique_id, uid0);
    EXPECT_EQ(f.router->Validate("g1", uid0, v0, f.now, &t0), RouteStatus::OK);

    PlacementVersion v1 = 0;
    uint64_t uid1 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.DeleteGraphPlacement("g1"));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE, &v1, &uid1));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_NE(uid1, uid0);
    // The old tuple is stale even though the dense GraphId slot is reused.
    RouteTarget told;
    EXPECT_EQ(f.router->Validate("g1", uid0, v0, f.now, &told), RouteStatus::STALE_PLACEMENT);
    EXPECT_EQ(told.unique_id, uid1);
    RouteTarget t1;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t1), RouteStatus::OK);
    EXPECT_EQ(t1.unique_id, uid1);
    EXPECT_EQ(f.router->Validate("g1", uid1, v1, f.now, &t1), RouteStatus::OK);
}

// Review finding 6: a warmed cache must not bypass shard-health/endpoint state.
TEST_F(TestRouter, CacheInvalidatedByShardChange) {
    AutoCleanDir cleaner("./test_router_shardchg");
    Fixture f;
    f.Init("./test_router_shardchg");
    f.locator->endpoint = "";  // fall back to the shard's registered endpoint
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0, "127.0.0.1:29092")));
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 0, PlacementState::ACTIVE));
        EXPECT_TRUE(batch.Commit());
    }
    RouteTarget t;
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.endpoint, "127.0.0.1:29092");

    // Mark the shard OFFLINE after warming the cache: the next resolve must not
    // use the cached endpoint.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->SetShardState(batch, 0, ShardState::OFFLINE));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::NO_HEALTHY_SHARD);

    // Back ONLINE with a new endpoint; the shard config-version change
    // invalidates the cached endpoint.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->RegisterShard(batch, MakeShard(0, "127.0.0.1:29099")));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_EQ(f.router->Resolve("g1", f.now, &t), RouteStatus::OK);
    EXPECT_EQ(t.endpoint, "127.0.0.1:29099");
}
