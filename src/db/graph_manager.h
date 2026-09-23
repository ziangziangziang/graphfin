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

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fma-common/timed_task.h"

#include "core/global_config.h"
#include "core/kv_store.h"
#include "core/lightning_graph.h"
#include "core/managed_object.h"

#include "db/acl.h"
#include "db/db.h"

namespace lgraph {
class Galaxy;

/**
 * @brief   Catalog of every REGISTERED graph: name -> DBConfig, readable
 *          without opening any graph storage. This is what makes ~100k
 *          registered graphs cheap: startup reads only these rows and a graph
 *          is opened lazily on first access.
 *
 *          Shared across GraphManager copies (the Galaxy copy-on-write) so the
 *          copy is O(1) regardless of the registered graph count. Thread-safe:
 *          all access is under Galaxy::graphs_lock_, except Size() which takes
 *          its own read lock.
 */
class GraphCatalog {
 public:
    void Put(const std::string& name, const DBConfig& config) {
        std::unique_lock<std::shared_mutex> l(mtx_);
        configs_[name] = config;
    }

    bool Remove(const std::string& name) {
        std::unique_lock<std::shared_mutex> l(mtx_);
        return configs_.erase(name) > 0;
    }

    bool GetConfig(const std::string& name, DBConfig& config) const {
        std::shared_lock<std::shared_mutex> l(mtx_);
        auto it = configs_.find(name);
        if (it == configs_.end()) return false;
        config = it->second;
        return true;
    }

    bool Exists(const std::string& name) const {
        std::shared_lock<std::shared_mutex> l(mtx_);
        return configs_.find(name) != configs_.end();
    }

    size_t Size() const {
        std::shared_lock<std::shared_mutex> l(mtx_);
        return configs_.size();
    }

    std::map<std::string, DBConfig> ListConfigs() const {
        std::shared_lock<std::shared_mutex> l(mtx_);
        return std::map<std::string, DBConfig>(configs_.begin(), configs_.end());
    }

    void Clear() {
        std::unique_lock<std::shared_mutex> l(mtx_);
        configs_.clear();
    }

 private:
    mutable std::shared_mutex mtx_;
    std::unordered_map<std::string, DBConfig> configs_;
};

class GraphManager {
 public:
    struct Config {
        bool durable = false;
        bool load_plugins = true;
        int plugin_subprocess_max_idle_seconds = 600;
        FullTextIndexOptions ft_index_options;
        bool enable_realtime_count = true;
        // 0 means no application-level limit on REGISTERED graphs.
        size_t max_graphs = 0;
        // Maximum number of graphs physically open (LMDB env + validator
        // thread + ~3 fds each). Registered graphs beyond this are opened
        // lazily on first access and evicted LRU.
        size_t max_open_graphs = 1000;
        // Evict graphs idle longer than this many seconds (0 = only evict when
        // the open count exceeds max_open_graphs).
        int graph_idle_timeout_s = 900;
        // Admission policy when max_open_graphs is reached and every open graph
        // has outstanding references: wait up to this many seconds for a lease
        // to be released, then fail the open with a retryable error instead of
        // exceeding the configured bound. 0 = fail immediately.
        double open_graph_admission_timeout_s = 30.0;

        Config() {}
        explicit Config(const GlobalConfig& gc)
            : durable(gc.durable),
              load_plugins(true),
              plugin_subprocess_max_idle_seconds(gc.subprocess_max_idle_seconds),
              ft_index_options(gc.ft_index_options),
              enable_realtime_count(gc.enable_realtime_count),
              max_graphs(static_cast<size_t>(gc.max_graphs < 0 ? 0 : gc.max_graphs)),
              max_open_graphs(static_cast<size_t>(
                  gc.max_open_graphs < 0 ? 1 : gc.max_open_graphs)),
              graph_idle_timeout_s(gc.graph_idle_timeout_s),
              open_graph_admission_timeout_s(
                  gc.graph_open_admission_timeout_s < 0 ? 0.0
                                                        : gc.graph_open_admission_timeout_s) {}
    };

    struct ModGraphActions {
        bool mod_desc = false;
        std::string desc;
        bool mod_size = false;
        size_t max_size = 0;
    };

    /** Lifecycle metrics, process-global (shared across manager copies). */
    struct Metrics {
        std::atomic<int64_t> cold_opens{0};
        std::atomic<int64_t> cache_hits{0};
        std::atomic<int64_t> cache_misses{0};
        std::atomic<int64_t> evictions{0};
        std::atomic<int64_t> evict_skipped_refs{0};
    };

 private:
    typedef GCRefCountedPtr<LightningGraph> GcDb;

    /** One physically open graph. All fields are guarded by graphs_lock_. */
    struct GraphEntry {
        GcDb graph;
        int64_t last_access_s = 0;
    };

    // The catalog is shared across manager copies so the copy-on-write in
    // Galaxy stays O(max_open_graphs) rather than O(registered graphs).
    std::shared_ptr<GraphCatalog> catalog_;
    std::shared_ptr<KvTable> table_;
    std::unordered_map<std::string, GraphEntry> open_graphs_;
    std::string parent_dir_;
    Config config_;
    std::shared_ptr<Metrics> metrics_;
    // Protects open_graphs_, the LRU order, and catalog mutations.
    mutable KillableRWLock lock_;

    std::string GenNewGraphSubDir();

    void StoreConfig(KvTransaction& txn, const std::string& name, const DBConfig& config);

    /** Constructs the LightningGraph for a registered graph and inserts it
     *  into the open set. Caller holds graphs_lock_ write. Evicts LRU graphs
     *  with no outstanding references while over max_open_graphs. */
    void OpenGraphInternal(const std::string& name, const DBConfig& config);

    /** Evict one evictable graph if at capacity. Returns true when there is
     *  room to open another graph (<= max_open_graphs). Caller holds lock_. */
    bool EnsureRoomLocked();

    /** Closes and removes open graphs until open_graphs_.size() < limit.
     *  Skips graphs with outstanding references. Caller holds graphs_lock_
     *  write. */
    void EvictOverLimit(size_t limit);

 public:
    GraphManager() = default;
    GraphManager(const GraphManager& rhs);
    GraphManager& operator=(const GraphManager& rhs);

    // called at program start; reads the graph catalog only, opens no graph
    void Init(KvStore* store, KvTransaction& txn, const std::string& table_name,
              const std::string& parent_dir, const Config& config);

    // register a graph (metadata only; the graph opens lazily on first access)
    bool CreateGraph(KvTransaction& txn, const std::string& name, const DBConfig& config,
                     DBConfig* stored = nullptr);

    // create a graph with data; opens it once to flush the db secret
    bool CreateGraphWithData(KvTransaction& txn, const std::string& name, const DBConfig& config,
                     const std::string& data_file_path, DBConfig* stored = nullptr);

    // del graph info from kv; returns the open graph (if any) so the caller
    // can destroy it once outstanding references drain
    lgraph::GraphManager::GcDb DelGraph(KvTransaction& txn, const std::string& name);

    bool ModGraph(KvTransaction& txn, const std::string& name, const ModGraphActions& actions,
                  DBConfig* stored = nullptr);

    // every registered graph, from the catalog (not from open storage)
    std::map<std::string, DBConfig> ListGraphs() const;

    size_t RegisteredGraphCount() const;

    size_t OpenGraphCount() const;

    /** Config of a registered graph, read from the catalog.
     *  @throws InputError if the graph is not registered. */
    DBConfig GetGraphConfig(const std::string& name) const;

    /** Opens the graph if closed, then returns a scoped reference.
     *  Caller holds graphs_lock_ write. Used on the cold-open path.
     *  @throws InputError if the graph is not registered. */
    /** Opens the graph if closed, then returns a scoped reference.
     *  Takes graphs_lock_ read for a hit, write for a cold open.
     *  @throws InputError if the graph is not registered. */
    lgraph::ScopedRef<lgraph::LightningGraph> GetGraphRef(const std::string& graph);

    /** Caller holds graphs_lock_ write. Opens if closed, skips eviction. */
    lgraph::ScopedRef<lgraph::LightningGraph> GetOrOpenGraphRef(const std::string& graph);

    /** True if the graph is physically open (has a live LMDB environment).
     *  Caller holds graphs_lock_ (read or write). */
    bool GraphIsOpen(const std::string& graph) const;

    bool GraphExists(const std::string& graph) const;

    // reload the graph catalog from disk; opens no graph
    void ReloadFromDisk(KvStore* store, KvTransaction& txn, const std::string& table_name,
                        const std::string& parent_dir, const Config& config);

    // backup all registered graphs, one open at a time
    std::vector<std::string> Backup(const std::string& parent_dir);

    // closes all open graphs before destroy
    void CloseAllGraphs();

    /** Evict a single open graph from the cache if it has no outstanding
     *  references. Used by Backup to keep at most one graph physically open at
     *  a time instead of letting the open set grow to max_open_graphs. */
    void CloseGraph(const std::string& name);

    /** Evicts graphs idle longer than graph_idle_timeout_s. Called by the
     *  Galaxy-owned eviction task, which resolves this manager through the
     *  current Galaxy::graphs_ pointer under graphs_lock_. */
    void EvictIdleGraphs();

    // registers a graph config in the catalog; called after the KV commit
    void CatalogPut(const std::string& name, const DBConfig& config) {
        catalog_->Put(name, config);
    }

    // removes a graph from the catalog; called after the KV commit
    void CatalogRemove(const std::string& name) { catalog_->Remove(name); }

    const Metrics& GetMetrics() const { return *metrics_; }
};

inline void UpdateDBConfigWithGMConfig(DBConfig& dbc, const GraphManager::Config& gmc) {
    dbc.load_plugins = gmc.load_plugins;
    dbc.durable = gmc.durable;
    dbc.subprocess_max_idle_seconds = gmc.plugin_subprocess_max_idle_seconds;
    dbc.ft_index_options = gmc.ft_index_options;
    dbc.enable_realtime_count = gmc.enable_realtime_count;
}
}  // namespace lgraph
