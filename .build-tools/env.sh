#!/bin/bash
# Environment for the memory-constrained host build.
set -euo pipefail
R=/home/pdfexe/graphfin
export LD_LIBRARY_PATH="$R/deps/install/lib:$R/deps/install/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export JAVA_HOME="$R/deps/install/jdk11"
export PATH="$R/deps/install/bin:$JAVA_HOME/bin:$HOME/.local/bin:$PATH"
export CC=/usr/bin/gcc
export CXX=/usr/bin/g++
cd "$R"
