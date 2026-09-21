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
#include <string>

#include "cluster/cluster_control.h"
#include "cluster/router.h"
#include "cluster/shard_manager.h"

namespace lgraph {
namespace cluster {

/** How a routed request should be treated by this server. */
enum class Disposition {
    HANDLE_LOCALLY = 0,   // this server's shard hosts the graph: apply it
    FORWARD = 1,           // send to RouteTarget.endpoint
    REJECT_UNKNOWN_GRAPH,  // no such logical graph
    REJECT_NOT_ACTIVE,     // graph exists but is CREATING/MOVING/DELETING
    REJECT_NO_SHARD,       // placement exists but no healthy shard
    REJECT_STALE,          // caller's placement version is behind
};

const char* ToString(Disposition d);

/** Result of forwarding a request to another shard. */
struct ForwardResult {
    bool ok = false;
    std::string payload;  // response payload (e.g. serialized LGraphResponse)
    std::string error;
    bool forwarded = false;  // false when the request was not sent
};

/**
 * Sends a payload to a resolved shard endpoint. The production implementation
 * uses the existing brpc RPC client (`LGraphRequest`/`LGraphResponse`); tests
 * substitute a loopback recorder. Kept payload-opaque so the routing core
 * stays independent of protobuf wiring.
 */
class Forwarder {
 public:
    virtual ~Forwarder() = default;
    virtual ForwardResult Forward(const RouteTarget& target, const std::string& payload) = 0;
};

struct HandleResult {
    Disposition disposition = Disposition::REJECT_UNKNOWN_GRAPH;
    RouteTarget target;              // resolved (or current, for stale) target
    ForwardResult forwarded;         // valid only when disposition == FORWARD
    std::string reason;
};

/**
 * @brief   Server-side routing core (Phase 4C.2).
 *
 * This is the decision layer the server's request path will call
 * (`StateMachine::HandleRequest` gets a hook that extracts the logical graph
 * name and the placement version the caller saw, then calls Handle()):
 *
 * - unknown / non-active / no-shard  -> reject with a reason;
 * - graph hosted on this server's shard -> fence the sender's version at the
 *   receiving boundary, then HANDLE_LOCALLY (the caller applies the request);
 * - hosted elsewhere -> FORWARD to the resolved endpoint via Forwarder.
 *
 * Stateful only in the bounded router cache; no graph data moves through it.
 */
class ClusterRequestHandler {
 public:
    ClusterRequestHandler(ClusterMetaStore* store, ShardManager* shards, Router* router,
                          ClusterControl* control, Forwarder* forwarder, ShardId local_shard);

    /**
     * Route one request. (`expected_uid`, `seen_version`) is the incarnation +
     * epoch the caller routed with (both 0 when unknown, meaning "resolve
     * fresh"). The local write boundary always re-fences with the
     * authoritative tuple; a directly-reached obsolete destination still
     * refuses. `payload` is forwarded verbatim when the decision is FORWARD.
     * A UID mismatch (recreate) is REJECT_STALE even when the version
     * coincidentally matches.
     */
    HandleResult Handle(const std::string& graph, uint64_t expected_uid,
                        PlacementVersion seen_version, const std::string& payload,
                        int64_t now_ms);

 private:
    ClusterMetaStore* store_;
    ShardManager* shards_;
    Router* router_;
    ClusterControl* control_;
    Forwarder* forwarder_;
    ShardId local_shard_;
};

}  // namespace cluster
}  // namespace lgraph
