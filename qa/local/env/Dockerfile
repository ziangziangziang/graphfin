# Derived compile image.
#
# Base: the pinned graphfin-compile-arm64:base image (see ../images.lock).
#
# WHY THIS EXISTS
# ---------------
# The base image ships Cython 0.29.37, but src/cython/lgraph_db_python.py uses
# Cython 3.0 pure-Python-mode syntax (`@cython.cclass` with annotated cdef
# attributes), so the `lgraph_db_python` target fails with:
#
#   /workspace/src/cython/lgraph_db_python.py:95:52:
#     Cannot take address of Python object attribute 'db'
#   make[2]: *** [src/lgraph_db_python.cxx] Error 1
#
# The repository's own ci/images Dockerfile pins `cython==3.0.0a11`, which is an
# alpha release that was never published to PyPI, so it cannot be installed
# directly. Cython 3.0.0 final installs and runs correctly on this image's
# Python 3.6.9 and compiles the extension.
#
# This is the only change relative to the pinned base image. The base image is
# deliberately NOT mutated, so that the originally-pinned artifact stays
# byte-identical and the derived image has its own pinned ID.
#
# BUILD
# -----
#   cd <repo root>
#   # arm64 (the pinned baseline environment)
#   docker build -f qa/local/env/Dockerfile -t graphfin-compile-arm64:local .
#
#   # amd64 (CI runners; base image from the upstream registry)
#   docker build -f qa/local/env/Dockerfile \
#       --build-arg BASE_IMAGE=tugraph/tugraph-compile-centos7:latest \
#       -t graphfin-compile-amd64:local .
#
# Record the resulting `docker image inspect ... --format '{{.Id}}'` value in
# qa/local/images.lock under [compile_qa]. qa/local/container.sh verifies
# it on every build and benchmark run.

ARG BASE_IMAGE=graphfin-compile-arm64:base
FROM ${BASE_IMAGE}

RUN pip3 install --no-cache-dir cython==3.0.0 \
    && cython --version \
    && python3 -c "import sys; print('python', sys.version.split()[0])"
