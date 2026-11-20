/**
* Copyright 2024 AntGroup CO., Ltd.
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

#include <cstdlib>
#include "lgraph/lgraph.h"
#include "lgraph/lgraph_types.h"
#include "lgraph/lgraph_result.h"
#include "tools/json.hpp"
#include "./algo.h"

using json = nlohmann::json;
using namespace lgraph_api;
using namespace lgraph_api::olap;

extern "C" LGAPI bool GetSignature(SigSpec &sig_spec) {
    sig_spec.input_list = {
        {.name = "k", .index = 0, .type = LGraphType::INTEGER},
    };
    sig_spec.result_list = {
        {.name = "num_rings", .index = 0, .type = LGraphType::INTEGER},
        {.name = "num_vertices", .index = 1, .type = LGraphType::INTEGER},
        {.name = "num_edges", .index = 2, .type = LGraphType::INTEGER}
    };
    return true;
}

extern "C" LGAPI bool ProcessInTxn(Transaction &txn,
                                   const std::string &request,
                                   Result &response) {
    double start_time = get_time();
    int k = 3;
    LOG_DEBUG() << "input: " << request;
    try {
        json input = json::parse(request);
        parse_from_json(k, "k", input);
    } catch (std::exception &e) {
        response.ResetHeader({
            {"errMsg", LGraphType::STRING}
        });
        response.MutableRecord()->Insert(
            "errMsg",
            FieldData::String(std::string("error parsing json: ") + e.what()));
        return false;
    }

    // A ring only exists with respect to edge direction, and the core prunes on
    // in-degree, so the snapshot must keep in-edges separate from out-edges.
    OlapOnDB<Empty> graph(txn, SNAPSHOT_PARALLEL);
    auto prepare_cost = get_time() - start_time;
    start_time = get_time();
    auto num_rings = LocateCycleCore(graph, static_cast<size_t>(k));
    auto core_cost = get_time() - start_time;

    response.ResetHeader({
        {"num_rings", LGraphType::INTEGER},
        {"num_vertices", LGraphType::INTEGER},
        {"num_edges", LGraphType::INTEGER},
    });
    auto r = response.MutableRecord();
    r->Insert("num_rings", FieldData::Int64(static_cast<int64_t>(num_rings)));
    r->Insert("num_vertices", FieldData::Int64(static_cast<int64_t>(graph.NumVertices())));
    r->Insert("num_edges", FieldData::Int64(static_cast<int64_t>(graph.NumEdges())));

    LOG_DEBUG() << "prepare_cost: " << prepare_cost << " (s)\n"
                << "core_cost: " << core_cost << " (s)\n"
                << "response.Size: " << response.Size();
    return true;
}
