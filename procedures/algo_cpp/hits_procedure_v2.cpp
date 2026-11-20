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
        {.name = "iterations", .index = 0, .type = LGraphType::INTEGER},
        {.name = "delta", .index = 1, .type = LGraphType::DOUBLE},
    };
    sig_spec.result_list = {
        {.name = "node", .index = 0, .type = LGraphType::NODE},
        {.name = "authority", .index = 1, .type = LGraphType::DOUBLE},
        {.name = "hub", .index = 2, .type = LGraphType::DOUBLE}
    };
    return true;
}

extern "C" LGAPI bool ProcessInTxn(Transaction &txn,
                                   const std::string &request,
                                   Result &response) {
    double start_time = get_time();
    int iterations = 100;
    double delta = 0.0001;
    LOG_DEBUG() << "input: " << request;
    try {
        json input = json::parse(request);
        parse_from_json(iterations, "iterations", input);
        parse_from_json(delta, "delta", input);
    } catch (std::exception &e) {
        response.ResetHeader({
            {"errMsg", LGraphType::STRING}
        });
        response.MutableRecord()->Insert(
            "errMsg",
            FieldData::String(std::string("error parsing json: ") + e.what()));
        return false;
    }

    // HITS scores hubs and authorities from opposite edge directions, so the
    // snapshot has to keep them apart.
    OlapOnDB<Empty> graph(txn, SNAPSHOT_PARALLEL);
    auto prepare_cost = get_time() - start_time;
    start_time = get_time();
    auto authority = graph.AllocVertexArray<double>();
    auto hub = graph.AllocVertexArray<double>();
    HITSCore(graph, authority, hub, iterations, delta);
    auto core_cost = get_time() - start_time;

    start_time = get_time();
    response.ResetHeader({
        {"node", LGraphType::NODE},
        {"authority", LGraphType::DOUBLE},
        {"hub", LGraphType::DOUBLE},
    });

    response.Resize(authority.Size());
    graph.ProcessVertexInRange<char>(
        [&] (size_t vid) {
            auto r = response.At(vid);
            r->InsertVertexByID("node", graph.OriginalVid(vid));
            r->Insert("authority", FieldData::Double(authority[vid]));
            r->Insert("hub", FieldData::Double(hub[vid]));
            return 0;
        },
        0, authority.Size());
    auto output_cost = get_time() - start_time;

    LOG_DEBUG() << "prepare_cost: " << prepare_cost << " (s)\n"
                << "core_cost: " << core_cost << " (s)\n"
                << "output_cost: " << output_cost << " (s)\n"
                << "response.Size: " << response.Size();
    return true;
}
