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
        {.name = "src", .index = 0, .type = LGraphType::NODE},
        {.name = "dst", .index = 1, .type = LGraphType::NODE},
        {.name = "directed", .index = 2, .type = LGraphType::BOOLEAN},
    };
    sig_spec.result_list = {
        {.name = "length", .index = 0, .type = LGraphType::INTEGER}
    };
    return true;
}

extern "C" LGAPI bool ProcessInTxn(Transaction &txn,
                                   const std::string &request,
                                   Result &response) {
    double start_time = get_time();
    std::string src_node;
    std::string dst_node;
    bool directed = true;
    LOG_DEBUG() << "input: " << request;
    try {
        json input = json::parse(request);
        parse_from_json(src_node, "src", input);
        parse_from_json(dst_node, "dst", input);
        parse_from_json(directed, "directed", input);
    } catch (std::exception &e) {
        response.ResetHeader({
            {"errMsg", LGraphType::STRING}
        });
        response.MutableRecord()->Insert(
            "errMsg",
            FieldData::String(std::string("error parsing json: ") + e.what()));
        return false;
    }

    // Callers ask either "can this reach that" (directed) or "are they connected"
    // (undirected), and the two answers differ, so the choice is an argument rather
    // than a default buried in the algorithm.
    size_t flags = SNAPSHOT_PARALLEL;
    if (!directed) {
        flags |= SNAPSHOT_UNDIRECTED;
    }
    OlapOnDB<Empty> graph(txn, flags);
    auto prepare_cost = get_time() - start_time;
    start_time = get_time();
    std::pair<size_t, size_t> search_pair = {graph.MappedVid(GetVidFromNodeString(src_node)),
                                             graph.MappedVid(GetVidFromNodeString(dst_node))};
    auto length = SPSPCore(graph, search_pair);
    auto core_cost = get_time() - start_time;

    response.ResetHeader({
        {"length", LGraphType::INTEGER},
    });
    auto r = response.MutableRecord();
    r->Insert("length", FieldData::Int64(static_cast<int64_t>(length)));

    LOG_DEBUG() << "prepare_cost: " << prepare_cost << " (s)\n"
                << "core_cost: " << core_cost << " (s)\n"
                << "response.Size: " << response.Size();
    return true;
}
