/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// These headers are installed for clients. Include them before anything else so
// a missing transitive include is a build failure here rather than in a
// downstream project.
#include "lgraph/lgraph_spatial.h"
#include "tools/lgraph_log.h"

#include "./ut_utils.h"

TEST(TestPublicHeaders, ChangedHeadersCompileStandalone) {
    lgraph_api::SRID srid = lgraph_api::SRID::CARTESIAN;
    lgraph_api::Point<lgraph_api::Cartesian> point(1.0, 2.0, srid);
    EXPECT_FALSE(point.AsEWKB().empty());
    LOG_INFO() << "public header check";
}
