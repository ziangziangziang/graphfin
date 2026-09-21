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

    explicit MigrationManager(KvStore* store);
    MigrationManager(KvStore* store, Config config);

    void Init(KvTransaction& txn, bool create_if_not_exist);

    // ---- Transaction-owned batch (R2) -----------------------------------
    //
    // All mutations MUST go through Batch. Workers observe committed state
    // only; staged mutations are published to memory after the durable commit
    // succeeds, in one Commit() entry point with RAII abort. An aborted batch
    // leaves no in-memory migration behind.
    class Batch {
     public:
        Batch(MigrationManager* mgr, KvTransaction& kv);
        ~Batch();
        DISABLE_COPY(Batch);
        Batch(Batch&& other) noexcept;
        Batch& operator=(Batch&& other) noexcept = delete;

        /** Begin a migration. Fails if an ACTIVE migration already exists for uid. */
        bool Begin(uint64_t graph_uid, ShardId src, ShardId dst,
                   PlacementVersion placement_version, uint64_t* out_id = nullptr);
        /**
         * Attempt-bound transitions (R7). When `expected_id != 0` the record's
         * migration_id must match, so a stale worker holding an old attempt id
         * can never advance/fail/cancel/prune a newer attempt for the same
         * graph. Pass the id returned by Begin.
         */
        bool Advance(uint64_t graph_uid, MigrationState to, uint64_t expected_id = 0);
        /** Mark failed (with reason). Post-cutover states must use ROLLBACK. */
        bool Fail(uint64_t graph_uid, const std::string& reason, uint64_t expected_id = 0);
        /** Cancel a pre-cutover migration (moves it to FAILED). */
        bool Cancel(uint64_t graph_uid, uint64_t expected_id = 0);
        /** Remove a terminal record (audit cleanup). Never reuses the id. */
        bool Prune(uint64_t graph_uid, uint64_t expected_id = 0);
        /** Record data-phase progress (0..1) and bytes moved. */
        bool ReportProgress(uint64_t graph_uid, double fraction, uint64_t bytes_delta,
                            uint64_t expected_id = 0);
        /** Record a failure; `retried` bumps the retry counter as well. */
        bool RecordFailure(uint64_t graph_uid, bool retried, uint64_t expected_id = 0);

        // Transaction-local reads (see own writes, never expose uncommitted to
        // other readers).
        bool Get(uint64_t graph_uid, Record* out) const;

        bool Commit();
        void Abort();

     private:
        const Record* FindForRead(uint64_t uid) const;
        Record* FindForWrite(uint64_t uid);

        MigrationManager* mgr_ = nullptr;
        KvTransaction* kv_ = nullptr;
        std::unique_lock<std::mutex> writer_guard_;
        bool done_ = false;
        bool committed_ = false;
        // Staged writes (pending publication) + staged deletes.
        std::unordered_map<uint64_t, Record> staged_;
        std::unordered_map<uint64_t, bool> staged_deleted_;
    };

    Batch BeginBatch(KvTransaction& kv) { return Batch(this, kv); }

    // Committed-only reads: never observe uncommitted staging (R2).
    bool Get(uint64_t graph_uid, Record* out) const;
    std::vector<Record> ListActive() const;
    size_t ActiveCount() const;

    /** Override the clock (milliseconds) for deterministic tests. */
    void SetClock(std::function<int64_t()> clock);

    static bool IsTerminal(MigrationState s);
    static bool CanTransition(MigrationState from, MigrationState to);

  private:
    void PersistLocked(KvTransaction& txn, const Record& r);
    void DeleteRowLocked(KvTransaction& txn, uint64_t graph_uid);
    void PersistNextIdLocked(KvTransaction& txn, uint64_t next_id);
    int64_t Now() const;

    KvStore* store_ = nullptr;
    Config config_;
    std::unique_ptr<KvTable> table_;
    mutable std::mutex mtx_;
    // Serializes whole batches: mutation -> durable commit -> publication (R2).
    mutable std::mutex write_mtx_;
    std::unordered_map<uint64_t, Record> migrations_;  // graph_uid -> record
    uint64_t next_id_ = 1;
    std::function<int64_t()> clock_;

    friend class Batch;
};

/** One shard's load snapshot for rebalancing. */
struct RebalanceInput {
    ShardId shard = INVALID_SHARD_ID;
    size_t graphs = 0;
    uint32_t weight = 1;    // 0 treated as 1; drain semantics are out of scope
    double disk_used = 0.0;  // 0..1, tiebreak for equal loads (see planner)
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

}  // namespace cluster
}  // namespace lgraph
