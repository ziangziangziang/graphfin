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

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fma-common/binary_buffer.h"
#include "fma-common/binary_read_write_helper.h"

#include "core/kv_engine.h"

#include "cluster/cluster_types.h"

namespace lgraph {
namespace cluster {

// One graph migration through the well-known lifecycle. The graph is named by
// its immutable unique_id (never the process-local GraphId), so the record
// stays valid across reloads and restarts.
enum class MigrationState : uint8_t {
    NONE = 0,
    SCHEDULED = 1,
    SNAPSHOTTING = 2,
    COPYING = 3,
    CATCHING_UP = 4,
    PREPARE_CUTOVER = 5,
    CUTOVER = 6,
    VALIDATING = 7,
    CLEANUP = 8,
    COMPLETED = 9,
    FAILED = 10,
    ROLLBACK = 11,
};

const char* ToString(MigrationState s);

/**
 * @brief   Restartable, idempotent migration state machine (Phase 5 core).
 *
 * Owns a small table (one row per migrating graph) in the same meta store and
 * keeps a tiny in-memory mirror (a handful of concurrent migrations). Every
 * transition is validated against the allowed edges and persisted before it is
 * applied in memory, so a crash at any point can be resumed by reloading and
 * continuing from the persisted state.
 *
 * Memory: one row per ACTIVE migration (a handful); nothing scales with graph
 * count.
 */
class MigrationManager {
 public:
    struct Record {
        uint64_t migration_id = 0;
        uint64_t graph_uid = 0;
        ShardId src_shard = INVALID_SHARD_ID;
        ShardId dst_shard = INVALID_SHARD_ID;
        MigrationState state = MigrationState::NONE;
        PlacementVersion placement_version = 0;  // placement version at schedule
        int64_t started_ms = 0;
        int64_t updated_ms = 0;
        std::string error;  // failure reason, if FAILED
        // Progress/metrics (PROJECT.md §8). Persisted with the record so an
        // interrupted migration resumes with its accounting intact.
        uint64_t bytes_transferred = 0;
        double progress = 0.0;  // 0..1 fraction of the data phase
        uint32_t failures = 0;
        uint32_t retries = 0;

        template <typename StreamT>
        size_t Serialize(StreamT& stream) const {
            size_t n = 0;
            n += fma_common::BinaryWrite(stream, migration_id);
            n += fma_common::BinaryWrite(stream, graph_uid);
            n += fma_common::BinaryWrite(stream, src_shard);
            n += fma_common::BinaryWrite(stream, dst_shard);
            n += fma_common::BinaryWrite(stream, static_cast<uint8_t>(state));
            n += fma_common::BinaryWrite(stream, placement_version);
            n += fma_common::BinaryWrite(stream, started_ms);
            n += fma_common::BinaryWrite(stream, updated_ms);
            n += fma_common::BinaryWrite(stream, error);
            n += fma_common::BinaryWrite(stream, bytes_transferred);
            n += fma_common::BinaryWrite(stream, progress);
            n += fma_common::BinaryWrite(stream, failures);
            n += fma_common::BinaryWrite(stream, retries);
            return n;
        }

        template <typename StreamT>
        size_t Deserialize(StreamT& stream) {
            uint8_t st = 0;
            size_t n = 0;
            n += fma_common::BinaryRead(stream, migration_id);
            n += fma_common::BinaryRead(stream, graph_uid);
            n += fma_common::BinaryRead(stream, src_shard);
            n += fma_common::BinaryRead(stream, dst_shard);
            n += fma_common::BinaryRead(stream, st);
            n += fma_common::BinaryRead(stream, placement_version);
            n += fma_common::BinaryRead(stream, started_ms);
            n += fma_common::BinaryRead(stream, updated_ms);
            n += fma_common::BinaryRead(stream, error);
            state = static_cast<MigrationState>(st);
            n += fma_common::BinaryRead(stream, bytes_transferred);
            n += fma_common::BinaryRead(stream, progress);
            n += fma_common::BinaryRead(stream, failures);
            n += fma_common::BinaryRead(stream, retries);
            return n;
        }
    };

    struct Config {
        std::string migration_table = "_cluster_migration_";
    };

    explicit MigrationManager(KvStore* store, Config config = Config());

    void Init(KvTransaction& txn, bool create_if_not_exist);

    /** Begin a migration. Fails if an ACTIVE migration already exists for uid. */
    bool Begin(KvTransaction& txn, uint64_t graph_uid, ShardId src, ShardId dst,
               PlacementVersion placement_version, uint64_t* out_id = nullptr);

    /** Advance along the lifecycle (validated). Fails on an invalid transition. */
    bool Advance(KvTransaction& txn, uint64_t graph_uid, MigrationState to);

    /** Mark failed (with reason) from any non-terminal state. */
    bool Fail(KvTransaction& txn, uint64_t graph_uid, const std::string& reason);

    /** Cancel a pre-cutover migration (moves it to FAILED). */
    bool Cancel(KvTransaction& txn, uint64_t graph_uid);

    /** Remove a terminal record (audit cleanup). */
    bool Prune(KvTransaction& txn, uint64_t graph_uid);

    bool Get(uint64_t graph_uid, Record* out) const;
    std::vector<Record> ListActive() const;
    size_t ActiveCount() const;

    /** Record data-phase progress (0..1) and bytes moved. */
    bool ReportProgress(KvTransaction& txn, uint64_t graph_uid, double fraction,
                        uint64_t bytes_delta);
    /** Record a failure; `retried` bumps the retry counter as well. */
    bool RecordFailure(KvTransaction& txn, uint64_t graph_uid, bool retried);

    /** Override the clock (milliseconds) for deterministic tests. */
    void SetClock(std::function<int64_t()> clock);

    static bool IsTerminal(MigrationState s);
    static bool CanTransition(MigrationState from, MigrationState to);

/** One shard's load snapshot for rebalancing. */
struct RebalanceInput {
    ShardId shard = INVALID_SHARD_ID;
    size_t graphs = 0;
    uint32_t weight = 1;    // 0 treated as 1; drain semantics are out of scope
    double disk_used = 0.0;  // 0..1, currently a deterministic tiebreak only
};

/** One suggested unit of rebalancing: move a single graph src -> dst. */
struct RebalanceMove {
    ShardId src = INVALID_SHARD_ID;
    ShardId dst = INVALID_SHARD_ID;
};

/**
 * Greedy rebalancer: repeatedly move one graph from the highest-loaded shard
 * (graphs/weight) to the lowest-loaded *different* shard while it strictly
 * reduces the peak load, up to max_moves. Pure function — deterministic,
 * tie-breaks by lowest shard id. Runs against the live counts; the caller
 * applies moves via the migration lifecycle (Begin/Advance).
 */
std::vector<RebalanceMove> PlanRebalanceMoves(const std::vector<RebalanceInput>& shards,
                                              size_t max_moves);

 private:
    void PersistLocked(KvTransaction& txn, const Record& r);
    void DeleteRowLocked(KvTransaction& txn, uint64_t graph_uid);
    int64_t Now() const;

    KvStore* store_ = nullptr;
    Config config_;
    std::unique_ptr<KvTable> table_;
    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, Record> migrations_;  // graph_uid -> record
    uint64_t next_id_ = 1;
    std::function<int64_t()> clock_;
};

}  // namespace cluster
}  // namespace lgraph
