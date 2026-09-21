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

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/kv_engine.h"

#include "cluster/cluster_types.h"

namespace lgraph {
namespace cluster {

/**
 * @brief   Durable, memory-frugal catalog of the cluster control plane:
 *          shard registry + graph->shard placement.
 *
 * Phase 4A. This is the control-plane state that Phase 4's router reads to
 * resolve a logical graph to its shard. It is deliberately NOT the graph data
 * store — it holds only placement metadata.
 *
 * Memory model (RAM is scarce on the test fleet):
 *   * Placement records live in one flat `std::vector<GraphPlacement>`
 *     (16 B each), indexed by a dense GraphId. No per-graph strings or
 *     node-based maps.
 *   * Graph names live in a single byte arena; name->GraphId is an
 *     open-addressing table whose slots are 4-byte (graph_id+1) values, so
 *     ~4 B/bucket and no per-entry allocations.
 *   * The shard registry is tiny (a handful of shards) and kept in a map.
 *   * The authoritative bytes live in LMDB (three tables). The in-memory
 *     index holds only what the hot path needs and is rebuilt on startup.
 *
 * At 100k graphs the resident index is roughly: placements 1.6 MiB + name
 * arena ~2 MiB + offsets 0.4 MiB + hash buckets ~1 MiB ≈ 5 MiB, versus tens of
 * MiB for a naive `unordered_map<string, struct>`. See
 * docs/architecture/11-cluster-metadata.md.
 *
 * Thread-safety: public methods take the internal lock; callers still own the
 * KvTransaction used for durable writes.
 */
class ClusterMetaStore {
 public:
    struct Config {
        std::string shard_table = "_cluster_shard_";
        std::string graph_table = "_cluster_graph_";
        std::string meta_table = "_cluster_meta_";
    };

    ClusterMetaStore() = default;
    DISABLE_COPY(ClusterMetaStore);

    /**
     * Open (or create) the catalog tables and rebuild the in-memory index.
     *
     * @param store               meta KV store (LMDB), not owned.
     * @param txn                 transaction used to open the tables.
     * @param create_if_not_exist create the tables when absent.
     */
    void Init(KvStore* store, KvTransaction& txn, bool create_if_not_exist);
    void Init(KvStore* store, KvTransaction& txn, const Config& config,
              bool create_if_not_exist);

    // Staged (durable-written, not-yet-published) mutations. Pending is
    // per-Batch (transaction-owned), never store-global (R1).
    enum class PendingType : uint8_t {
        PUT_GRAPH = 0,
        DELETE_GRAPH = 1,
        REGISTER_SHARD = 2,
        REMOVE_SHARD = 3,
        SHARD_STATE = 4,
        SET_VERSION = 5,
    };
    struct Pending {
        PendingType type;
        std::string name;                    // PUT_GRAPH / DELETE_GRAPH
        GraphPlacement placement;            // PUT_GRAPH
        ShardInfo shard;                     // REGISTER_SHARD / SHARD_STATE (R10: full descriptor)
        ShardId shard_id = INVALID_SHARD_ID; // REMOVE_SHARD
        ConfigVersion version = 0;           // DELETE_GRAPH / SET_VERSION / SHARD_STATE
    };

    // ---- Committed reads (never observe uncommitted staging) ------------

    bool GetShard(ShardId id, ShardInfo* out) const;
    std::vector<ShardInfo> ListShards() const;
    size_t ShardCount() const;

    bool HasGraph(const std::string& name) const;

    bool GetGraphPlacement(const std::string& name, GraphPlacement* out,
                           GraphId* out_id = nullptr) const;
    bool GetGraphPlacement(GraphId id, GraphPlacement* out) const;
    bool GetGraphName(GraphId id, std::string* out) const;
    size_t GraphCount() const;

    /**
     * Visit every non-DELETED graph: fn(GraphId, name, placement).
     * Used by admin listing paths (Phase 4E). O(N); not for the hot path.
     */
    template <typename F>
    void ForEachGraph(F&& fn) const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        for (GraphId id = 0; id < static_cast<GraphId>(placements_.size()); id++) {
            if (placements_[id].State() == PlacementState::DELETED) continue;
            uint32_t len = 0;
            const char* data = NameOf(id, &len);
            fn(id, std::string(data, len), placements_[id]);
        }
    }

    /** Number of graphs currently placed on `shard`. */
    size_t GraphCountOnShard(ShardId shard) const;

    // ---- Transaction-owned batch ----------------------------------------
    //
    // All mutations MUST go through Batch. A Batch owns one KvTransaction,
    // holds the store-wide writer lock from construction until Commit/Abort,
    // stages mutations transaction-locally (never store-global), writes the
    // durable rows, and publishes to the in-memory index only after the
    // durable commit succeeds — in one Commit() entry point with RAII abort.
    //
    // This closes REVIEW R1: two interleaved batches can never share staged
    // state, because staging is per-Batch and only one Batch can be active
    // (writer-serialized) at a time. Readers observe committed state only.
    class Batch {
     public:
        Batch(ClusterMetaStore* store, KvTransaction& kv);
        ~Batch();
        DISABLE_COPY(Batch);
        Batch(Batch&& other) noexcept;
        Batch& operator=(Batch&& other) noexcept = delete;

        bool RegisterShard(const ShardInfo& info);
        bool RemoveShard(ShardId id);
        bool SetShardState(ShardId id, ShardState state);
        bool PutGraphPlacement(const std::string& name, ShardId shard,
                               PlacementState state,
                               PlacementVersion* out_version = nullptr,
                               uint64_t* out_uid = nullptr);
        bool DeleteGraphPlacement(const std::string& name);
        bool SetVersion(ConfigVersion v);

        // Transaction-local (read-your-writes) views for validation (R8).
        bool HasGraph(const std::string& name) const;
        bool GetGraphPlacement(const std::string& name, GraphPlacement* out,
                               GraphId* out_id = nullptr) const;
        size_t GraphCountOnShard(ShardId shard) const;
        bool GetShard(ShardId id, ShardInfo* out) const;

        // Durable commit followed by in-memory publication. Returns false on
        // durable-commit failure (nothing published). After Commit/Abort the
        // Batch must not be used again.
        bool Commit();
        void Abort();

        size_t StagedCount() const;
        bool Committed() const { return done_ && committed_; }

     private:
        ClusterMetaStore* store_ = nullptr;
        KvTransaction* kv_ = nullptr;
        std::unique_lock<std::mutex> writer_guard_;
        std::vector<Pending> pending_;
        ConfigVersion staged_version_ = 0;
        bool done_ = false;
        bool committed_ = false;
        // Transaction-local UIDs for staged puts. R3: a delete drops the
        // entry so a later recreate in the same batch allocates a fresh
        // incarnation UID; repeated puts without an intervening delete reuse.
        std::unordered_map<std::string, uint64_t> batch_uids_;
        std::unordered_map<std::string, GraphPlacement> batch_puts_;
        std::unordered_set<std::string> batch_deletes_;
        std::unordered_map<ShardId, ShardInfo> batch_shards_;
        std::unordered_map<ShardId, bool> batch_shard_removed_;

        bool FindPlacementForWrite(const std::string& name, GraphPlacement* out,
                                   bool* is_new) const;
    };

    Batch BeginBatch(KvTransaction& kv) { return Batch(this, kv); }

    // ---- Cluster version -------------------------------------------------

    ConfigVersion Version() const;

    /** Approximate resident bytes of the in-memory index (tests/metrics). */
    size_t MemoryFootprint() const;

 private:
    void Reload(KvTransaction& txn);
    void AdjustShardCount(ShardId shard, int delta);
    void ApplyPutGraph(const std::string& name, const GraphPlacement& placement);
    void ApplyDeleteGraph(const std::string& name, ConfigVersion version);
    void WriteVersion(KvTransaction& txn);
    void WriteNextUid(KvTransaction& txn);

    // Compact name index ---------------------------------------------------
    void IndexInsert(GraphId id, const char* data, size_t len);
    void IndexRebuild(const std::vector<std::pair<GraphId, std::string>>& names);
    void IndexResize(size_t new_bucket_count);
    bool IndexFind(const std::string& name, GraphId* id) const;
    const char* NameOf(GraphId id, uint32_t* len) const;

    KvStore* store_ = nullptr;
    Config config_;
    std::unique_ptr<KvTable> shard_table_;
    std::unique_ptr<KvTable> graph_table_;
    std::unique_ptr<KvTable> meta_table_;

    mutable std::shared_mutex mtx_;
    // Serializes whole batches: mutation -> durable commit -> publication.
    // Held by Batch from construction until Commit/Abort (R1).
    mutable std::mutex write_mtx_;

    std::vector<GraphPlacement> placements_;  // GraphId -> placement
    size_t live_count_ = 0;                   // placements not in DELETED state
    // Per-shard count of non-DELETED graphs, maintained incrementally so that
    // placement and admin queries are O(1) rather than O(N). Tiny: one entry
    // per shard.
    std::unordered_map<ShardId, size_t> shard_counts_;
    std::vector<uint32_t> name_offset_;       // GraphId -> offset in name_arena_
    std::vector<char> name_arena_;            // [uint32 len][bytes]
    std::vector<uint32_t> bucket_;            // open addressing, value = id+1
    size_t bucket_mask_ = 0;                  // bucket_.size() - 1

    std::unordered_map<ShardId, ShardInfo> shards_;

    ConfigVersion version_ = 0;
    // Monotonic allocator for immutable graph unique_ids. Persisted so ids are
    // never reused, even after restarts and deleted graphs.
    uint64_t next_uid_ = 0;

    friend class Batch;
};

}  // namespace cluster
}  // namespace lgraph
