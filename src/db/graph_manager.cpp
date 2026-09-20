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

#include <chrono>
#include <memory>
#include <random>

#include "fma-common/binary_buffer.h"
#include "fma-common/binary_read_write_helper.h"
#include "fma-common/encrypt.h"
#include "fma-common/utils.h"  // GetTime / SleepUs for admission waiting

#include "db/graph_manager.h"

// generate a subdir name for new graph
// the dir name is generated as Base16(timestamp) + 16-byte random string
std::string lgraph::GraphManager::GenNewGraphSubDir() {
    std::string subdir;
    size_t s = lgraph::_detail::GRAPH_SUBDIR_NAME_LEN;
    std::random_device r;
    std::default_random_engine e1(r());
    std::uniform_int_distribution<int> uniform_dist(0, 256);
    std::string buf(s, 0);
    for (size_t i = 0; i < buf.size(); i++) buf[i] = (char)uniform_dist(e1);
    subdir.append(fma_common::encrypt::Base16::Encode(buf));
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string tmps;
    tmps.assign((const char*)&t, sizeof(t));
    subdir.append(fma_common::encrypt::Base16::Encode(tmps));
    return subdir;
}

static inline std::string GetGraphActualDir(const std::string& parent_dir,
                                            const std::string& sub_dir) {
    return fma_common::StringFormatter::Format("{}/{}", parent_dir, sub_dir);
}

static inline int64_t NowSeconds() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

void lgraph::GraphManager::StoreConfig(lgraph::KvTransaction& txn, const std::string& name,
                                       const lgraph::DBConfig& config) {
    fma_common::BinaryBuffer buf;
    fma_common::BinaryWrite(buf, config);
    table_->SetValue(txn, lgraph::Value::ConstRef(name),
                     lgraph::Value(buf.GetBuf(), buf.GetSize()));
}

lgraph::GraphManager::GraphManager(const GraphManager& rhs)
    : catalog_(rhs.catalog_),
      table_(rhs.table_),
      open_graphs_(rhs.open_graphs_),
      parent_dir_(rhs.parent_dir_),
      config_(rhs.config_),
      metrics_(rhs.metrics_),
      lock_() {}

lgraph::GraphManager& lgraph::GraphManager::operator=(const GraphManager& rhs) {
    if (this != &rhs) {
        catalog_ = rhs.catalog_;
        table_ = rhs.table_;
        open_graphs_ = rhs.open_graphs_;
        parent_dir_ = rhs.parent_dir_;
        config_ = rhs.config_;
        metrics_ = rhs.metrics_;
    }
    return *this;
}

void lgraph::GraphManager::Init(KvStore* store, KvTransaction& txn,
                                const std::string& table_name, const std::string& parent_dir,
                                const Config& config) {
    if (!catalog_) catalog_.reset(new GraphCatalog());
    if (!metrics_) metrics_.reset(new Metrics());
    ReloadFromDisk(store, txn, table_name, parent_dir, config);
}

bool lgraph::GraphManager::CreateGraph(KvTransaction& txn, const std::string& name,
                                       const DBConfig& config, DBConfig* stored) {
    lgraph::CheckValidDescLength(config.desc.size());
    if (catalog_->Exists(name)) return false;
    lgraph::CheckValidGraphNum(catalog_->Size() + 1, config_.max_graphs);
    DBConfig real_config = config;
    UpdateDBConfigWithGMConfig(real_config, config_);
    real_config.name = name;
    real_config.dir = GenNewGraphSubDir();
    StoreConfig(txn, name, real_config);
    if (stored) *stored = real_config;
    return true;
}

bool lgraph::GraphManager::CreateGraphWithData(KvTransaction& txn, const std::string& name,
                                       const DBConfig& config,
                                       const std::string& data_file_path, DBConfig* stored) {
    lgraph::CheckValidGraphNum(catalog_->Size() + 1, config_.max_graphs);
    DBConfig real_config = config;
    UpdateDBConfigWithGMConfig(real_config, config_);
    real_config.name = name;
    real_config.db_size = _detail::DEFAULT_GRAPH_SIZE;
    real_config.create_if_not_exist = true;
    real_config.dir = GenNewGraphSubDir();
    StoreConfig(txn, name, real_config);
    std::string secret = real_config.dir;
    real_config.dir = GetGraphActualDir(parent_dir_, real_config.dir);
    std::unique_ptr<LightningGraph> graph(new LightningGraph(real_config));
    graph->FlushDbSecret(secret);
    graph->Close();
    graph.reset();
    std::string new_file_path = GetGraphActualDir(real_config.dir, "data.mdb");
    std::error_code ec;
    std::filesystem::copy_file(data_file_path, new_file_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;
    if (stored) *stored = real_config;
    return true;
}

lgraph::GraphManager::GcDb lgraph::GraphManager::DelGraph(KvTransaction& txn,
                                                          const std::string& name) {
    table_->DeleteKey(txn, lgraph::Value::ConstRef(name));
    auto it = open_graphs_.find(name);
    if (it == open_graphs_.end()) return GcDb();
    GcDb db = it->second.graph;
    open_graphs_.erase(it);
    return db;
}

bool lgraph::GraphManager::ModGraph(KvTransaction& txn, const std::string& name,
                                    const ModGraphActions& actions, DBConfig* stored) {
    DBConfig config;
    if (!catalog_->GetConfig(name, config))
        THROW_CODE(InputError, "No such graph: {}", name);
    if (actions.mod_size && actions.max_size != config.db_size) {
        lgraph::CheckValidGraphSize(actions.max_size);
        config.db_size = actions.max_size;
        open_graphs_.erase(name);
    }
    if (actions.mod_desc) config.desc = actions.desc;
    StoreConfig(txn, name, config);
    if (stored) *stored = config;
    return true;
}

std::map<std::string, lgraph::DBConfig> lgraph::GraphManager::ListGraphs() const {
    return catalog_->ListConfigs();
}

size_t lgraph::GraphManager::RegisteredGraphCount() const { return catalog_->Size(); }

size_t lgraph::GraphManager::OpenGraphCount() const { return open_graphs_.size(); }

lgraph::DBConfig lgraph::GraphManager::GetGraphConfig(const std::string& name) const {
    DBConfig config;
    if (!catalog_->GetConfig(name, config))
        THROW_CODE(InputError, "No such graph: {}", name);
    return config;
}

bool lgraph::GraphManager::GraphExists(const std::string& graph) const {
    return catalog_->Exists(graph);
}

lgraph::ScopedRef<lgraph::LightningGraph> lgraph::GraphManager::GetGraphRef(
    const std::string& graph) {
    DBConfig config;
    if (!catalog_->GetConfig(graph, config))
        THROW_CODE(InputError, "No such graph: {}", graph);
    {
        AutoReadLock l(lock_, GetMyThreadId());
        auto it = open_graphs_.find(graph);
        if (it != open_graphs_.end()) {
            it->second.last_access_s = NowSeconds();
            metrics_->cache_hits++;
            return it->second.graph.GetScopedRef();
        }
        metrics_->cache_misses++;
    }
    // Bounded admission: if the open set is at max_open_graphs and every graph
    // is pinned by an outstanding lease, wait up to the configured timeout for
    // a lease to be released, then fail with a retryable error rather than
    // silently exceeding the configured bound.
    const double deadline = fma_common::GetTime() + config_.open_graph_admission_timeout_s;
    while (true) {
        {
            AutoWriteLock l(lock_, GetMyThreadId());
            auto it = open_graphs_.find(graph);
            if (it != open_graphs_.end()) {
                it->second.last_access_s = NowSeconds();
                return it->second.graph.GetScopedRef();
            }
            if (EnsureRoomLocked()) {
                OpenGraphInternal(graph, config);
                metrics_->cold_opens++;
                return open_graphs_[graph].graph.GetScopedRef();
            }
        }
        if (fma_common::GetTime() >= deadline) {
            THROW_CODE(Timeout,
                       "Open-graph capacity reached and all {} open graphs are in use; "
                       "retry later or raise max_open_graphs",
                       config_.max_open_graphs);
        }
        fma_common::SleepUs(1000);
    }
}

lgraph::ScopedRef<lgraph::LightningGraph> lgraph::GraphManager::GetOrOpenGraphRef(
    const std::string& graph) {
    DBConfig config;
    if (!catalog_->GetConfig(graph, config))
        THROW_CODE(InputError, "No such graph: {}", graph);
    AutoWriteLock l(lock_, GetMyThreadId());
    auto it = open_graphs_.find(graph);
    if (it == open_graphs_.end()) {
        if (!EnsureRoomLocked()) {
            THROW_CODE(Timeout,
                       "Open-graph capacity reached and all {} open graphs are in use; "
                       "retry later or raise max_open_graphs",
                       config_.max_open_graphs);
        }
        OpenGraphInternal(graph, config);
        metrics_->cold_opens++;
    } else {
        it->second.last_access_s = NowSeconds();
    }
    return open_graphs_[graph].graph.GetScopedRef();
}

bool lgraph::GraphManager::GraphIsOpen(const std::string& graph) const {
    return open_graphs_.find(graph) != open_graphs_.end();
}

void lgraph::GraphManager::ReloadFromDisk(KvStore* store, KvTransaction& txn,
                                          const std::string& table_name,
                                          const std::string& parent_dir, const Config& config) {
    parent_dir_ = parent_dir;
    config_ = config;
    if (!catalog_) catalog_.reset(new GraphCatalog());
    if (!metrics_) metrics_.reset(new Metrics());
    table_ = store->OpenTable(txn, table_name, true, ComparatorDesc::DefaultComparator());
    if (table_->GetKeyCount(txn) == 0) {
        DBConfig config;
        config.name = _detail::DEFAULT_GRAPH_DB_NAME;
        config.dir = GenNewGraphSubDir();
        UpdateDBConfigWithGMConfig(config, config_);
        StoreConfig(txn, _detail::DEFAULT_GRAPH_DB_NAME, config);
    }
    catalog_->Clear();
    auto it = table_->GetIterator(txn);
    for (it->GotoFirstKey(); it->IsValid(); it->Next()) {
        const std::string& graph_name = it->GetKey().AsString();
        const Value& value = it->GetValue();
        DBConfig conf;
        fma_common::BinaryBuffer buf(value.Data(), value.Size());
        if (fma_common::BinaryRead(buf, conf) != value.Size()) {
            throw std::runtime_error("Failed to read DB config for graph " + graph_name);
        }
        conf.dir = GetGraphActualDir(parent_dir_, conf.dir);
        // create_if_not_exist = true is REQUIRED for lazy loading: a newly
        // registered graph has no directory until it is first accessed. With
        // false, any access to a never-opened graph fails with
        // "does not contain valid data". CheckDbSecret still catches a
        // mismatched directory (corruption).
        conf.create_if_not_exist = true;
        conf.durable = config_.durable;
        conf.load_plugins = config_.load_plugins;
        catalog_->Put(graph_name, conf);
    }
    for (auto it2 = open_graphs_.begin(); it2 != open_graphs_.end();) {
        if (!catalog_->Exists(it2->first)) {
            it2 = open_graphs_.erase(it2);
        } else {
            ++it2;
        }
    }
}

std::vector<std::string> lgraph::GraphManager::Backup(const std::string& backup_parent_dir) {
    std::vector<std::string> ret;
    for (auto& kv : catalog_->ListConfigs()) {
        const std::string& name = kv.first;
        LOG_INFO() << "Backup subgraph " << name;
        std::string graph_dir;
        {
            // Scope the ScopedRef so the graph's reference is released as soon as
            // the backup completes; then CloseGraph evicts it from the open set.
            // This keeps at most one graph physically open during a backup of N
            // graphs, instead of letting the open set grow to max_open_graphs.
            auto g = GetOrOpenGraphRef(name);
            std::string sub_dir = fma_common::FilePath(g->GetConfig().dir).Name();
            graph_dir = GetGraphActualDir(backup_parent_dir, sub_dir);
            ret.push_back(graph_dir + "/data.mdb");
            if (!fma_common::file_system::MkDir(graph_dir)) {
                LOG_WARN() << "Error backing up graph " << name << ": cannot create dir "
                           << graph_dir;
                throw std::runtime_error("Error backing up graph [" + name +
                                         "]: cannot create dir " + graph_dir);
            }
            g->Backup(graph_dir);
        }
        CloseGraph(name);
    }
    return ret;
}

void lgraph::GraphManager::CloseGraph(const std::string& name) {
    AutoWriteLock l(lock_, GetMyThreadId());
    auto it = open_graphs_.find(name);
    if (it == open_graphs_.end()) return;
    if (it->second.graph.HasOutstandingRefs()) {
        LOG_WARN() << "CloseGraph: graph " << name
                   << " still has outstanding references, skipping";
        return;
    }
    open_graphs_.erase(it);
}

void lgraph::GraphManager::CloseAllGraphs() {
    std::mutex m;
    std::condition_variable cv;
    size_t n_destroyed = 0;
    size_t n_opened = 0;
    for (auto& kv : open_graphs_) {
        if (kv.second.graph) {
            n_opened++;
            kv.second.graph.Assign(nullptr, nullptr, [&]() {
                std::lock_guard<std::mutex> l(m);
                n_destroyed++;
                cv.notify_all();
            });
        }
    }
    std::unique_lock<std::mutex> l(m);
    while (n_opened != n_destroyed) cv.wait(l);
    open_graphs_.clear();
}

bool lgraph::GraphManager::EnsureRoomLocked() {
    // Evict at most one LRU graph that has no outstanding lease. Returns true
    // if there is room to open another graph after any eviction. Never evicts a
    // graph with outstanding references, so active transactions are preserved;
    // the caller decides how long to wait when no graph can be evicted.
    while (open_graphs_.size() >= config_.max_open_graphs) {
        auto best = open_graphs_.end();
        for (auto it = open_graphs_.begin(); it != open_graphs_.end(); ++it) {
            if (it->second.graph.HasOutstandingRefs()) {
                metrics_->evict_skipped_refs++;
                continue;
            }
            if (best == open_graphs_.end() ||
                it->second.last_access_s < best->second.last_access_s) {
                best = it;
            }
        }
        if (best == open_graphs_.end()) return false;  // all pinned
        open_graphs_.erase(best);
        metrics_->evictions++;
        return true;
    }
    return true;
}

void lgraph::GraphManager::OpenGraphInternal(const std::string& name, const DBConfig& config) {
    // Caller guarantees room via EnsureRoomLocked().
    // config.dir may be relative (from CreateGraph) or absolute (from
    // ReloadFromDisk). Always derive the full path from parent_dir_ and the
    // basename, which is also the db secret.
    std::string sub_dir = fma_common::FilePath(config.dir).Name();
    DBConfig conf = config;
    conf.dir = GetGraphActualDir(parent_dir_, sub_dir);
    std::string secret = sub_dir;
    std::unique_ptr<LightningGraph> graph(new LightningGraph(conf));
    if (!graph->CheckDbSecret(secret)) throw std::runtime_error("DB corruptted.");
    GraphEntry entry;
    entry.graph = GcDb(graph.release());
    entry.last_access_s = NowSeconds();
    open_graphs_[name] = std::move(entry);
}

void lgraph::GraphManager::EvictIdleGraphs() {
    int64_t idle_s = config_.graph_idle_timeout_s;
    if (idle_s <= 0) return;
    AutoWriteLock l(lock_, GetMyThreadId());
    int64_t now = NowSeconds();
    for (auto it = open_graphs_.begin(); it != open_graphs_.end();) {
        if (now - it->second.last_access_s < idle_s) {
            ++it;
            continue;
        }
        if (it->second.graph.HasOutstandingRefs()) {
            metrics_->evict_skipped_refs++;
            ++it;
            continue;
        }
        it = open_graphs_.erase(it);
        metrics_->evictions++;
    }
}
