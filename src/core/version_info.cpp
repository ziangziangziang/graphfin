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

#include "core/version_info.h"

// Deliberately the only TU including the generated header: every value here
// changes with the git commit, the compiler, or the Python library, and
// including it anywhere else reintroduces commit-per-commit rebuilds.
#include "core/version.h"

namespace lgraph {
namespace version {
namespace {

// The generator emits "" when the information is unavailable (no .git for
// the main or web trees). Preserve the historical "unknown" rendering.
const char* OrUnknown(const char* s) { return (s && *s) ? s : "unknown"; }

}  // namespace

const char* GitBranch() { return OrUnknown(GIT_BRANCH); }
const char* GitCommitHash() { return OrUnknown(GIT_COMMIT_HASH); }
const char* WebGitCommitHash() { return OrUnknown(WEB_GIT_COMMIT_HASH); }
const char* CxxCompilerId() { return OrUnknown(CXX_COMPILER_ID); }
const char* CxxCompilerVersion() { return OrUnknown(CXX_COMPILER_VERSION); }
const char* PythonLibVersion() { return OrUnknown(PYTHON_LIB_VERSION); }
int Major() { return LGRAPH_VERSION_MAJOR; }
int Minor() { return LGRAPH_VERSION_MINOR; }
int Patch() { return LGRAPH_VERSION_PATCH; }
std::string ShortVersion() {
    return std::to_string(Major()) + "." + std::to_string(Minor()) + "." +
           std::to_string(Patch());
}

}  // namespace version
}  // namespace lgraph
