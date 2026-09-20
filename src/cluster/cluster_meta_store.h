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

    // ---- Shard registry -------------------------------------------------

    /** Register or overwrite a shard descriptor. Bumps the cluster version. */
    bool RegisterShard(KvTransaction& txn, const ShardInfo& info);
    /** Remove a shard. Fails if any ACTIVE/not-DELETED graph still lives there. */
    bool RemoveShard(KvTransaction& txn, ShardId id);
    bool SetShardState(KvTransaction& txn, ShardId id, ShardState state);
    bool GetShard(ShardId id, ShardInfo* out) const;
    std::vector<ShardInfo> ListShards() const;
    size_t ShardCount() const;

    // ---- Graph placement ------------------------------------------------

    /**
     * Create or update the placement of `name`. A new name is assigned the next
     * dense GraphId; an existing name has its placement replaced and its
     * placement_version bumped. Bumps the cluster version.
     */
    bool PutGraphPlacement(KvTransaction& txn, const std::string& name, ShardId shard,
                           PlacementState state, PlacementVersion* out_version = nullptr);
    bool DeleteGraphPlacement(KvTransaction& txn, const std::string& name);
    bool HasGraph(const std::string& name) const;

    bool GetGraphPlacement(const std::string& name, GraphPlacement* out,
                           GraphId* out_id = nullptr) const;
    bool GetGraphPlacement(GraphId id, GraphPlacement* out) const;
    bool GetGraphName(GraphId id, std::string* out) const;
    size_t GraphCount() const;

    /** Number of graphs currently placed on `shard`. */
    size_t GraphCountOnShard(ShardId shard) const;

    // ---- Cluster version -------------------------------------------------

    ConfigVersion Version() const;
    bool SetVersion(KvTransaction& txn, ConfigVersion v);

    // ---- Staged publication ---------------------------------------------
    //
    // Mutators above change ONLY the durable tables and record a pending
    // change; the in-memory index is updated by CommitStaged() (call after
    // txn->Commit()) and discarded by RollbackStaged() (call on abort or
    // commit failure). This guarantees readers never observe an uncommitted
    // placement and that a failed/aborted transaction cannot leave the index
    // inconsistent with storage.
    bool CommitStaged();
    void RollbackStaged();
    bool HasStaged() const;
    size_t StagedCount() const;

    /** Approximate resident bytes of the in-memory index (tests/metrics). */
    size_t MemoryFootprint() const;

 private:
    void Reload(KvTransaction& txn);
    void AdjustShardCount(ShardId shard, int delta);
    void ApplyPutGraph(const std::string& name, const GraphPlacement& placement);
    void ApplyDeleteGraph(const std::string& name, ConfigVersion version);
    void WriteVersion(KvTransaction& txn);

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

    // Staged (durable-written, not-yet-published) mutations.
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
        ShardInfo shard;                     // REGISTER_SHARD
        ShardId shard_id = INVALID_SHARD_ID; // REMOVE_SHARD / SHARD_STATE
        ShardState shard_state = ShardState::OFFLINE;  // SHARD_STATE
        ConfigVersion version = 0;           // DELETE_GRAPH / SET_VERSION
    };
    std::vector<Pending> pending_;
    // Version that WOULD be current if the staged batch commits. Mutators
    // assign per-mutation versions from this so they are monotonic even before
    // publication.
    ConfigVersion staged_version_ = 0;
};

}  // namespace cluster
}  // namespace lgraph
