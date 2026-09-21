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

#include "gtest/gtest.h"

#include "core/kv_store.h"
#include "cluster/migration_manager.h"

#include "./test_tools.h"
#include "./ut_utils.h"

using namespace lgraph;
using namespace lgraph::cluster;

class TestMigrationManager : public TuGraphTest {};

namespace {

const MigrationState kPath[] = {
    MigrationState::SNAPSHOTTING, MigrationState::COPYING, MigrationState::CATCHING_UP,
    MigrationState::PREPARE_CUTOVER, MigrationState::CUTOVER, MigrationState::VALIDATING,
    MigrationState::CLEANUP,        MigrationState::COMPLETED,
};

}  // namespace

TEST_F(TestMigrationManager, FullLifecycleAndValidation) {
    AutoCleanDir cleaner("./test_migration");
    auto store = std::make_unique<LMDBKvStore>("./test_migration");
    auto mgr = std::make_unique<MigrationManager>(store.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr->Init(*txn, true);
        txn->Commit();
    }

    // Invalid begins.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_FALSE(mgr->Begin(*txn, 0, 0, 1, 0));       // no zero uid
        EXPECT_FALSE(mgr->Begin(*txn, 7, 0, 0, 0));       // src == dst
        EXPECT_FALSE(mgr->Begin(*txn, 7, 0, INVALID_SHARD_ID, 0));
        txn->Abort();
    }

    uint64_t id = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr->Begin(*txn, 7, 0, 1, 42, &id));
        EXPECT_GT(id, 0u);
        txn->Commit();
    }
    MigrationManager::Record r;
    EXPECT_TRUE(mgr->Get(7, &r));
    EXPECT_EQ(r.state, MigrationState::SCHEDULED);
    EXPECT_EQ(r.src_shard, 0u);
    EXPECT_EQ(r.dst_shard, 1u);
    EXPECT_EQ(r.placement_version, 42u);
    EXPECT_EQ(mgr->ActiveCount(), 1u);

    // Re-begin while active is rejected.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_FALSE(mgr->Begin(*txn, 7, 0, 1, 43));
        txn->Abort();
    }

    // Skipping states is rejected; the linear path succeeds.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_FALSE(mgr->Advance(*txn, 7, MigrationState::CUTOVER));
        txn->Abort();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        for (auto st : kPath) {
            EXPECT_TRUE(mgr->Advance(*txn, 7, st)) << (int)st;
        }
        txn->Commit();
    }
    EXPECT_TRUE(mgr->Get(7, &r));
    EXPECT_EQ(r.state, MigrationState::COMPLETED);
    EXPECT_EQ(mgr->ActiveCount(), 0u);
}

TEST_F(TestMigrationManager, CancelFailRollbackAndPersistence) {
    AutoCleanDir cleaner("./test_migration_ctrl");
    auto store = std::make_unique<LMDBKvStore>("./test_migration_ctrl");
    auto mgr = std::make_unique<MigrationManager>(store.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr->Init(*txn, true);
        txn->Commit();
    }

    // Cancel a pre-cutover migration.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr->Begin(*txn, 9, 0, 1, 1));
        EXPECT_TRUE(mgr->Advance(*txn, 9, MigrationState::SNAPSHOTTING));
        EXPECT_TRUE(mgr->Cancel(*txn, 9));
        txn->Commit();
    }
    MigrationManager::Record r;
    EXPECT_TRUE(mgr->Get(9, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);
    EXPECT_EQ(r.error, "cancelled");

    // Fail with a reason.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr->Begin(*txn, 11, 0, 1, 5));
        EXPECT_TRUE(mgr->Fail(*txn, 11, "disk full"));
        txn->Commit();
    }
    EXPECT_TRUE(mgr->Get(11, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);
    EXPECT_EQ(r.error, "disk full");

    // Cancel past cutover is refused; rollback must be used.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr->Begin(*txn, 13, 0, 1, 7));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::SNAPSHOTTING));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::COPYING));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::CATCHING_UP));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::PREPARE_CUTOVER));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::CUTOVER));
        EXPECT_FALSE(mgr->Cancel(*txn, 13));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::ROLLBACK));
        EXPECT_TRUE(mgr->Advance(*txn, 13, MigrationState::FAILED));
        txn->Commit();
    }
    EXPECT_TRUE(mgr->Get(13, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);

    // Prune a terminal record so the graph can migrate again later.
    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_FALSE(mgr->Prune(*txn, 999));  // unknown
        EXPECT_TRUE(mgr->Prune(*txn, 13));
        txn->Commit();
    }
    EXPECT_FALSE(mgr->Get(13, &r));

    // A fresh manager on the same store sees the persisted state (restartable).
    {
        auto store2 = std::make_unique<LMDBKvStore>("./test_migration_ctrl");
        auto mgr2 = std::make_unique<MigrationManager>(store2.get());
        auto txn = store2->CreateWriteTxn(false);
        mgr2->Init(*txn, true);
        txn->Commit();
        EXPECT_TRUE(mgr2->Get(9, &r));
        EXPECT_EQ(r.state, MigrationState::FAILED);
        EXPECT_TRUE(mgr2->Get(11, &r));
        EXPECT_EQ(r.state, MigrationState::FAILED);
        EXPECT_FALSE(mgr2->Get(13, &r));  // pruned
        EXPECT_EQ(mgr2->ActiveCount(), 0u);
    }
}

TEST_F(TestMigrationManager, ProgressAndFailureAccounting) {
    AutoCleanDir cleaner("./test_migration_metrics");
    auto store = std::make_unique<LMDBKvStore>("./test_migration_metrics");
    auto mgr = std::make_unique<MigrationManager>(store.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr->Init(*txn, true);
        txn->Commit();
    }
    EXPECT_FALSE(mgr->ReportProgress(*store->CreateWriteTxn(false), 99, 0.5, 10));

    {
        auto txn = store->CreateWriteTxn(false);
        EXPECT_TRUE(mgr->Begin(*txn, 7, 0, 1, 1));
        EXPECT_TRUE(mgr->ReportProgress(*txn, 7, 0.25, 1000));
        EXPECT_TRUE(mgr->ReportProgress(*txn, 7, 2.5, 500));  // clamped to 1.0
        EXPECT_TRUE(mgr->RecordFailure(*txn, 7, true));
        EXPECT_TRUE(mgr->RecordFailure(*txn, 7, false));
        txn->Commit();
    }
    MigrationManager::Record r;
    EXPECT_TRUE(mgr->Get(7, &r));
    EXPECT_DOUBLE_EQ(r.progress, 1.0);
    EXPECT_EQ(r.bytes_transferred, 1500u);
    EXPECT_EQ(r.failures, 2u);
    EXPECT_EQ(r.retries, 1u);
    EXPECT_FALSE(mgr->ReportProgress(*store->CreateWriteTxn(false), 12345, 0.5, 1));
}

namespace {

RebalanceInput MakeInput(ShardId s, size_t g, uint32_t w, double d) {
    RebalanceInput in;
    in.shard = s;
    in.graphs = g;
    in.weight = w;
    in.disk_used = d;
    return in;
}

}  // namespace

TEST_F(TestMigrationManager, PlanRebalanceMoves) {
    // Empty and singleton inputs produce nothing.
    EXPECT_TRUE(PlanRebalanceMoves({}, 10).empty());
    {
        std::vector<RebalanceInput> one;
        one.push_back(MakeInput(0, 5, 1, 0.0));
        EXPECT_TRUE(PlanRebalanceMoves(one, 10).empty());
    }

    // Already balanced: no moves.
    std::vector<RebalanceInput> even;
    even.push_back(MakeInput(0, 5, 1, 0.0));
    even.push_back(MakeInput(1, 5, 1, 0.0));
    EXPECT_TRUE(PlanRebalanceMoves(even, 10).empty());

    // 10 vs 0 (equal weights) -> exactly 5 moves, halving the peak each time.
    std::vector<RebalanceInput> skewed;
    skewed.push_back(MakeInput(0, 10, 1, 0.0));
    skewed.push_back(MakeInput(1, 0, 1, 0.0));
    auto moves = PlanRebalanceMoves(skewed, 100);
    ASSERT_EQ(moves.size(), 5u);
    for (const auto& m : moves) {
        EXPECT_EQ(m.src, 0u);
        EXPECT_EQ(m.dst, 1u);
    }

    // max_moves caps the plan.
    EXPECT_EQ(PlanRebalanceMoves(skewed, 2).size(), 2u);

    // Weights respected: shard 1 with weight 3 absorbs more.
    std::vector<RebalanceInput> weighted;
    weighted.push_back(MakeInput(0, 8, 1, 0.0));
    weighted.push_back(MakeInput(1, 0, 3, 0.0));
    auto wm = PlanRebalanceMoves(weighted, 100);
    EXPECT_FALSE(wm.empty());
    for (const auto& m : wm) {
        EXPECT_EQ(m.src, 0u);
        EXPECT_EQ(m.dst, 1u);
    }

    // Disk breaks load ties among destinations: shard 1 and 2 both sit at
    // load 4, so the emptier disk (shard 2) is chosen. The move also strictly
    // reduces the peak (8 -> 7).
    std::vector<RebalanceInput> disks;
    disks.push_back(MakeInput(0, 8, 1, 0.5));
    disks.push_back(MakeInput(1, 4, 1, 0.9));
    disks.push_back(MakeInput(2, 4, 1, 0.1));
    auto dm = PlanRebalanceMoves(disks, 1);
    ASSERT_EQ(dm.size(), 1u);
    EXPECT_EQ(dm[0].src, 0u);
    EXPECT_EQ(dm[0].dst, 2u);
}
