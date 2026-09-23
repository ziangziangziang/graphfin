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

#include <initializer_list>
#include <memory>
#include <string>

#include "gtest/gtest.h"

#include "core/kv_store.h"
#include "cluster/cluster_meta_store.h"
#include "cluster/shard_manager.h"
#include "cluster/router.h"
#include "cluster/request_router.h"

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestRequestRouter : public TuGraphTest {};

namespace {

ShardInfo MakeShard(ShardId id) {
    ShardInfo s;
    s.shard_id = id;
    s.state = ShardState::ONLINE;
    s.name = "shard-" + std::to_string(id);
    s.endpoints = {"127.0.0.1:2909" + std::to_string(id)};
    return s;
}

class MockLocator : public ShardLocator {
 public:
    std::string LocateLeader(ShardId) override { return endpoint; }
    std::string endpoint = "leader:1";
};

class LoopbackForwarder : public Forwarder {
 public:
    ForwardResult Forward(const RouteTarget& t, const std::string& payload) override {
        calls++;
        last_target = t;
        last_payload = payload;
        ForwardResult r;
        r.ok = true;
        r.forwarded = true;
        r.payload = "applied@" + t.endpoint;
        return r;
    }
    int calls = 0;
    RouteTarget last_target;
    std::string last_payload;
};

class FailingForwarder : public Forwarder {
 public:
    ForwardResult Forward(const RouteTarget&, const std::string&) override {
        ForwardResult r;
        r.ok = false;
        r.forwarded = true;
        r.error = "connection refused";
        return r;
    }
};

struct Fixture {
    std::unique_ptr<LMDBKvStore> store;
    std::unique_ptr<ClusterMetaStore> ms;
    std::unique_ptr<ShardManager> mgr;
    std::unique_ptr<MockLocator> locator;
    std::unique_ptr<Router> router;
    std::unique_ptr<ClusterControl> control;
    std::unique_ptr<LoopbackForwarder> fwd;
    std::unique_ptr<ClusterRequestHandler> handler;
    int64_t now = 1000;

    // local_shard selects which shard "this server" belongs to.
    void Init(const std::string& dir, ShardId local_shard) {
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
        fwd.reset(new LoopbackForwarder());
        handler.reset(new ClusterRequestHandler(ms.get(), mgr.get(), router.get(),
                                                control.get(), fwd.get(), local_shard));
    }

    void RegisterShards(std::initializer_list<ShardId> ids) {
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        for (ShardId id : ids) EXPECT_TRUE(mgr->RegisterShard(batch, MakeShard(id)));
        EXPECT_TRUE(batch.Commit());
    }

    PlacementVersion Place(const std::string& name, ShardId shard) {
        PlacementVersion v = 0;
        auto txn = store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(ms.get(), *txn);
        if (!batch.PutGraphPlacement(name, shard, PlacementState::ACTIVE, &v)) {
            ADD_FAILURE() << "PutGraphPlacement failed for " << name;
            batch.Abort();
        } else {
            EXPECT_TRUE(batch.Commit());
        }
        return v;
    }
};

}  // namespace

TEST_F(TestRequestRouter, UnknownAndNotActive) {
    AutoCleanDir cleaner("./test_req_router_basic");
    Fixture f;
    f.Init("./test_req_router_basic", 0);
    f.RegisterShards({0, 1});

    auto r = f.handler->Handle("nope", 0, 0, "payload", f.now);
    EXPECT_EQ(r.disposition, Disposition::REJECT_UNKNOWN_GRAPH);

    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g2", 0, PlacementState::CREATING));
        EXPECT_TRUE(batch.Commit());
    }
    r = f.handler->Handle("g2", 0, 0, "payload", f.now);
    EXPECT_EQ(r.disposition, Disposition::REJECT_NOT_ACTIVE);
}

TEST_F(TestRequestRouter, LocalHandledAndRemoteForwarded) {
    AutoCleanDir cleaner("./test_req_router_route");
    Fixture f;
    f.Init("./test_req_router_route", /*local_shard=*/0);
    f.RegisterShards({0, 1});
    f.Place("local", 0);
    f.Place("away", 1);

    auto local = f.handler->Handle("local", 0, 0, "payload", f.now);
    EXPECT_EQ(local.disposition, Disposition::HANDLE_LOCALLY);
    EXPECT_EQ(local.target.shard_id, 0u);

    auto remote = f.handler->Handle("away", 0, 0, "payload", f.now);
    EXPECT_EQ(remote.disposition, Disposition::FORWARD);
    EXPECT_EQ(f.fwd->calls, 1);
    EXPECT_EQ(f.fwd->last_target.shard_id, 1u);
    EXPECT_EQ(f.fwd->last_payload, "payload");
    EXPECT_TRUE(remote.forwarded.ok);
    EXPECT_EQ(remote.forwarded.payload, "applied@leader:1");
}

TEST_F(TestRequestRouter, StaleCallerRejectedBeforeForward) {
    AutoCleanDir cleaner("./test_req_router_stale");
    Fixture f;
    f.Init("./test_req_router_stale", /*local_shard=*/0);
    f.RegisterShards({0, 1});

    PlacementVersion v1 = f.Place("g1", 0);
    GraphPlacement gp1;
    EXPECT_EQ(f.control->GetPlacement("g1", &gp1), ControlStatus::OK);
    auto first = f.handler->Handle("g1", gp1.unique_id, v1, "payload", f.now);
    EXPECT_EQ(first.disposition, Disposition::HANDLE_LOCALLY);

    // Move the graph to shard 1 (new version).
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::ACTIVE, &v2));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_GT(v2, v1);

    // Replaying the old request (seen == v1) is rejected, not forwarded with
    // the stale version; the current target is returned for the retry.
    auto stale = f.handler->Handle("g1", gp1.unique_id, v1, "payload", f.now);
    EXPECT_EQ(stale.disposition, Disposition::REJECT_STALE);
    EXPECT_EQ(stale.target.shard_id, 1u);
    EXPECT_EQ(stale.target.version, v2);
    EXPECT_EQ(f.fwd->calls, 0);  // no forward happened

    // A caller that already saw the move is forwarded correctly.
    GraphPlacement gp2;
    EXPECT_EQ(f.control->GetPlacement("g1", &gp2), ControlStatus::OK);
    auto fresh = f.handler->Handle("g1", gp2.unique_id, v2, "payload", f.now);
    EXPECT_EQ(fresh.disposition, Disposition::FORWARD);
    EXPECT_EQ(f.fwd->calls, 1);
    EXPECT_EQ(f.fwd->last_target.shard_id, 1u);
}

// R3/R4: recreate retires the routed incarnation; a future epoch from a
// lagging-or-racing sender is rejected without forwarding.
TEST_F(TestRequestRouter, RecreatedIncarnationAndFutureEpochRejected) {
    AutoCleanDir cleaner("./test_req_router_r3r4");
    Fixture f;
    f.Init("./test_req_router_r3r4", /*local_shard=*/0);
    f.RegisterShards({0, 1});
    PlacementVersion v1 = f.Place("g1", 0);
    GraphPlacement gp1;
    EXPECT_EQ(f.control->GetPlacement("g1", &gp1), ControlStatus::OK);
    auto first = f.handler->Handle("g1", gp1.unique_id, v1, "payload", f.now);
    EXPECT_EQ(first.disposition, Disposition::HANDLE_LOCALLY);

    // Recreate on the other shard: old UID rejected, fresh UID forwarded.
    PlacementVersion v2 = 0;
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.DeleteGraphPlacement("g1"));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(batch.PutGraphPlacement("g1", 1, PlacementState::ACTIVE, &v2));
        EXPECT_TRUE(batch.Commit());
    }
    GraphPlacement gp2;
    EXPECT_EQ(f.control->GetPlacement("g1", &gp2), ControlStatus::OK);
    EXPECT_NE(gp2.unique_id, gp1.unique_id);
    auto stale_uid = f.handler->Handle("g1", gp1.unique_id, v1, "payload", f.now);
    EXPECT_EQ(stale_uid.disposition, Disposition::REJECT_STALE);
    EXPECT_EQ(stale_uid.target.unique_id, gp2.unique_id);
    EXPECT_EQ(f.fwd->calls, 0);
    auto fresh = f.handler->Handle("g1", gp2.unique_id, v2, "payload", f.now);
    EXPECT_EQ(fresh.disposition, Disposition::FORWARD);
    EXPECT_EQ(f.fwd->calls, 1);

    // Future epoch with the right UID is still rejected (sender must refresh).
    auto future = f.handler->Handle("g1", gp2.unique_id, v2 + 100, "payload", f.now);
    EXPECT_EQ(future.disposition, Disposition::REJECT_STALE);
    EXPECT_EQ(f.fwd->calls, 1);  // no additional forward
}

TEST_F(TestRequestRouter, ForwardErrorSurfaced) {
    AutoCleanDir cleaner("./test_req_router_err");
    Fixture f;
    f.Init("./test_req_router_err", /*local_shard=*/0);
    f.RegisterShards({0, 1});
    f.Place("away", 1);

    FailingForwarder failing;
    ClusterRequestHandler h(f.ms.get(), f.mgr.get(), f.router.get(), f.control.get(),
                            &failing, 0);
    auto r = h.Handle("away", 0, 0, "payload", f.now);
    EXPECT_EQ(r.disposition, Disposition::FORWARD);
    EXPECT_TRUE(r.forwarded.forwarded);
    EXPECT_FALSE(r.forwarded.ok);
    EXPECT_EQ(r.forwarded.error, "connection refused");
}

TEST_F(TestRequestRouter, UnhealthyShardNotRouted) {
    AutoCleanDir cleaner("./test_req_router_health");
    Fixture f;
    f.Init("./test_req_router_health", /*local_shard=*/0);
    f.RegisterShards({0, 1});
    f.Place("away", 1);

    // The destination goes offline (and its heartbeat lapses): no forward.
    {
        auto txn = f.store->CreateWriteTxn(false);
        ClusterMetaStore::Batch batch(f.ms.get(), *txn);
        EXPECT_TRUE(f.mgr->SetShardState(batch, 1, ShardState::OFFLINE));
        EXPECT_TRUE(batch.Commit());
    }
    auto r = f.handler->Handle("away", 0, 0, "payload", f.now + 40000);
    EXPECT_EQ(r.disposition, Disposition::REJECT_NO_SHARD);
    EXPECT_EQ(f.fwd->calls, 0);
}
