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

#include <string>

namespace lgraph {
namespace version {

// Build metadata accessors. The values come from the generated
// core/version.h (git hash, compiler versions), which changes on every
// commit. These declarations are stable on purpose: version_info.cpp is the
// ONLY translation unit that includes the generated header, so a new commit
// invalidates one object file instead of the ~239 that include defs.h.
// Callers must use these functions, never the generated macros directly.
const char* GitBranch();
const char* GitCommitHash();
const char* WebGitCommitHash();
const char* CxxCompilerId();
const char* CxxCompilerVersion();
const char* PythonLibVersion();
int Major();
int Minor();
int Patch();
// "major.minor.patch", e.g. "4.5.2".
std::string ShortVersion();

}  // namespace version
}  // namespace lgraph
