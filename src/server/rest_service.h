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
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>

namespace lgraph {

class GlobalConfig;
class StateMachine;

/**
 * @brief The part of the REST server that the process bootstrap needs.
 *
 * The concrete server lives in restful/server/rest_server.h and drags the whole
 * cpprest header closure into every translation unit that includes it.  The
 * bootstrap only starts and stops the server, so it talks to this interface and
 * lets restful/server/rest_server.cpp own the cpprest types.
 */
class IRestServer {
 public:
    virtual ~IRestServer() = default;
    virtual void Stop() = 0;
};

/**
 * @brief Create the REST server for the given state machine.
 */
std::unique_ptr<IRestServer> CreateRestServer(StateMachine *state_machine,
                                              const std::shared_ptr<GlobalConfig> &config);

/**
 * @brief Size the cpprest worker pool.  A limit of 0 keeps the default.
 */
void InitRestThreadPool(int thread_limit);

}  // namespace lgraph
