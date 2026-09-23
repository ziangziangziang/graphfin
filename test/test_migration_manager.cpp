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
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Begin(0, 0, 1, 0));       // no zero uid
        EXPECT_FALSE(batch.Begin(7, 0, 0, 0));       // src == dst
        EXPECT_FALSE(batch.Begin(7, 0, INVALID_SHARD_ID, 0));
        batch.Abort();
    }

    uint64_t id = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 42, &id));
        EXPECT_GT(id, 0u);
        EXPECT_TRUE(batch.Commit());
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
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Begin(7, 0, 1, 43));
        batch.Abort();
    }

    // Skipping states is rejected; the linear path succeeds.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Advance(7, MigrationState::CUTOVER));
        batch.Abort();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        for (auto st : kPath) {
            EXPECT_TRUE(batch.Advance(7, st)) << (int)st;
        }
        EXPECT_TRUE(batch.Commit());
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
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(9, 0, 1, 1));
        EXPECT_TRUE(batch.Advance(9, MigrationState::SNAPSHOTTING));
        EXPECT_TRUE(batch.Cancel(9));
        EXPECT_TRUE(batch.Commit());
    }
    MigrationManager::Record r;
    EXPECT_TRUE(mgr->Get(9, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);
    EXPECT_EQ(r.error, "cancelled");

    // Fail with a reason.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(11, 0, 1, 5));
        EXPECT_TRUE(batch.Fail(11, "disk full"));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_TRUE(mgr->Get(11, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);
    EXPECT_EQ(r.error, "disk full");

    // Cancel past cutover is refused; rollback must be used.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(13, 0, 1, 7));
        EXPECT_TRUE(batch.Advance(13, MigrationState::SNAPSHOTTING));
        EXPECT_TRUE(batch.Advance(13, MigrationState::COPYING));
        EXPECT_TRUE(batch.Advance(13, MigrationState::CATCHING_UP));
        EXPECT_TRUE(batch.Advance(13, MigrationState::PREPARE_CUTOVER));
        EXPECT_TRUE(batch.Advance(13, MigrationState::CUTOVER));
        EXPECT_FALSE(batch.Cancel(13));
        EXPECT_TRUE(batch.Advance(13, MigrationState::ROLLBACK));
        EXPECT_TRUE(batch.Advance(13, MigrationState::FAILED));
        EXPECT_TRUE(batch.Commit());
    }
    EXPECT_TRUE(mgr->Get(13, &r));
    EXPECT_EQ(r.state, MigrationState::FAILED);

    // Prune a terminal record so the graph can migrate again later.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Prune(999));  // unknown
        EXPECT_TRUE(batch.Prune(13));
        EXPECT_TRUE(batch.Commit());
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
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.ReportProgress(99, 0.5, 10));
        batch.Abort();
    }

    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 1));
        EXPECT_TRUE(batch.ReportProgress(7, 0.25, 1000));
        EXPECT_TRUE(batch.ReportProgress(7, 2.5, 500));  // clamped to 1.0
        EXPECT_TRUE(batch.RecordFailure(7, true));
        EXPECT_TRUE(batch.RecordFailure(7, false));
        EXPECT_TRUE(batch.Commit());
    }
    MigrationManager::Record r;
    EXPECT_TRUE(mgr->Get(7, &r));
    EXPECT_DOUBLE_EQ(r.progress, 1.0);
    EXPECT_EQ(r.bytes_transferred, 1500u);
    EXPECT_EQ(r.failures, 2u);
    EXPECT_EQ(r.retries, 1u);
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.ReportProgress(12345, 0.5, 1));
        batch.Abort();
    }
}

// R7: transitions bind to the attempt id; the allocator survives Prune;
// post-cutover failures must roll back instead of going terminal directly.
TEST_F(TestMigrationManager, AttemptBindingAndAllocatorSurvivesPrune) {
    AutoCleanDir cleaner("./test_migration_r7");
    auto store = std::make_unique<LMDBKvStore>("./test_migration_r7");
    auto mgr = std::make_unique<MigrationManager>(store.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr->Init(*txn, true);
        txn->Commit();
    }
    uint64_t id1 = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 42, &id1));
        EXPECT_GT(id1, 0u);
        EXPECT_TRUE(batch.Commit());
    }
    // Stale attempt id cannot advance another attempt's record.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Advance(7, MigrationState::SNAPSHOTTING, id1 + 100));
        EXPECT_TRUE(batch.Advance(7, MigrationState::SNAPSHOTTING, id1));
        EXPECT_TRUE(batch.Commit());
    }
    // Drive to CUTOVER, then: direct Fail is rejected post-cutover; the
    // ROLLBACK path is required.
    for (auto st :
         {MigrationState::COPYING, MigrationState::CATCHING_UP, MigrationState::PREPARE_CUTOVER,
          MigrationState::CUTOVER}) {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Advance(7, st, id1));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Fail(7, "late failure", id1));
        EXPECT_FALSE(batch.Cancel(7, id1));
        EXPECT_TRUE(batch.Advance(7, MigrationState::ROLLBACK, id1));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Advance(7, MigrationState::FAILED, id1));
        EXPECT_TRUE(batch.Commit());
    }
    // Prune with the wrong attempt id fails; correct prune succeeds but the
    // allocator never reuses id1.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_FALSE(batch.Prune(7, id1 + 100));
        EXPECT_TRUE(batch.Prune(7, id1));
        EXPECT_TRUE(batch.Commit());
    }
    uint64_t id2 = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 43, &id2));
        EXPECT_GT(id2, id1);
        EXPECT_TRUE(batch.Commit());
    }
    // Reopen: the allocator row survived, so a third Begin keeps increasing.
    {
        auto store2 = std::make_unique<LMDBKvStore>("./test_migration_r7");
        auto mgr2 = std::make_unique<MigrationManager>(store2.get());
        auto txn = store2->CreateWriteTxn(false);
        mgr2->Init(*txn, true);
        txn->Commit();
        MigrationManager::Record r;
        EXPECT_TRUE(mgr2->Get(7, &r));
        EXPECT_EQ(r.migration_id, id2);
    }
    uint64_t id3 = 0;
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        // id2's record is still active; a second Begin is refused.
        EXPECT_FALSE(batch.Begin(7, 0, 1, 44, &id3));
        batch.Abort();
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Fail(7, "cleanup", id2));
        EXPECT_TRUE(batch.Prune(7, id2));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 44, &id3));
        EXPECT_GT(id3, id2);
        EXPECT_TRUE(batch.Commit());
    }
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

// R2: staged migration state is invisible until Commit and vanishes on Abort.
// Workers observe committed state only; a reopened manager must agree.
TEST_F(TestMigrationManager, AbortLeavesNoVisibleMigration) {
    AutoCleanDir cleaner("./test_migration_r2");
    auto store = std::make_unique<LMDBKvStore>("./test_migration_r2");
    auto mgr = std::make_unique<MigrationManager>(store.get());
    {
        auto txn = store->CreateWriteTxn(false);
        mgr->Init(*txn, true);
        txn->Commit();
    }
    // Begin then abort: no record visible, before or after reopen.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 42));
        // Staged write visible inside the batch (read-your-writes)...
        MigrationManager::Record staged;
        EXPECT_TRUE(batch.Get(7, &staged));
        EXPECT_EQ(staged.state, MigrationState::SCHEDULED);
        // ...but not to committed readers.
        MigrationManager::Record committed;
        EXPECT_FALSE(mgr->Get(7, &committed));
        EXPECT_EQ(mgr->ActiveCount(), 0u);
        batch.Abort();
    }
    MigrationManager::Record r;
    EXPECT_FALSE(mgr->Get(7, &r));
    EXPECT_EQ(mgr->ActiveCount(), 0u);

    // Advance then abort: committed state unchanged.
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Begin(7, 0, 1, 42));
        EXPECT_TRUE(batch.Commit());
    }
    {
        auto txn = store->CreateWriteTxn(false);
        MigrationManager::Batch batch(mgr.get(), *txn);
        EXPECT_TRUE(batch.Advance(7, MigrationState::SNAPSHOTTING));
        // Committed reader still sees SCHEDULED.
        EXPECT_TRUE(mgr->Get(7, &r));
        EXPECT_EQ(r.state, MigrationState::SCHEDULED);
        batch.Abort();
    }
    EXPECT_TRUE(mgr->Get(7, &r));
    EXPECT_EQ(r.state, MigrationState::SCHEDULED);

    // Reopened manager agrees (no phantom durable record from aborts).
    {
        auto store2 = std::make_unique<LMDBKvStore>("./test_migration_r2");
        auto mgr2 = std::make_unique<MigrationManager>(store2.get());
        auto txn = store2->CreateWriteTxn(false);
        mgr2->Init(*txn, true);
        txn->Commit();
        EXPECT_TRUE(mgr2->Get(7, &r));
        EXPECT_EQ(r.state, MigrationState::SCHEDULED);
        EXPECT_EQ(mgr2->ActiveCount(), 1u);
    }
}
