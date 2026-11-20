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

#include "cypher/execution_plan/ops/op.h"
#include "lgraph/lgraph_result.h"
#include "lgraph/lgraph_types.h"
#include "lgraph_api/result_element.h"
#include "resultset/record.h"
#include "cypher/execution_plan/ops/op_produce_results.h"

namespace cypher {

class ProduceResultsCol : public OpBase {
    enum {
        Uninitialized,
        RefreshAfterPass,
        Resetted,
        Consuming,
    } state_;
    lgraph_api::NODEMAP node_map_;
    lgraph_api::RELPMAP relp_map_;
    std::shared_ptr<DataChunk> final_r;

 public:
    ProduceResultsCol() : OpBase(OpType::PRODUCE_RESULTS, "Produce Results") {
        state_ = Uninitialized;
    }

    OpResult Initialize(RTContext *ctx) override {
        if (!children.empty()) {
            children[0]->Initialize(ctx);
        }
        columnar_ = std::make_shared<DataChunk>();
        final_r = std::make_shared<DataChunk>();
        return OP_OK;
    }

    /* ProduceResults next operation
     * called each time a new result record is required */
    OpResult RealConsume(RTContext *ctx) override;
    /* Restart */
    OpResult ResetImpl(bool complete) override {
        if (complete) state_ = Uninitialized;
        return OP_OK;
    }

    std::string ToString() const override {
        std::string str(name);
        return str;
    }

    CYPHER_DEFINE_VISITABLE()

    CYPHER_DEFINE_CONST_VISITABLE()
};
}  // namespace cypher
