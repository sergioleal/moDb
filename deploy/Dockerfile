# syntax=docker/dockerfile:1.7

# Build stage: toolchain + source → binário modb (Fase 13C).
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 ninja-build git ca-certificates python3-pip \
    && pip3 install --break-system-packages 'cmake>=3.30' \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY apps ./apps
COPY examples ./examples
COPY benchmarks ./benchmarks
COPY tests ./tests
COPY fuzz ./fuzz
ENV CXX=g++-14 CC=gcc-14
RUN cmake --preset release -DMODB_ALLOW_CXX23_FALLBACK=ON \
    && cmake --build --preset release --target modb_cli \
    && strip /src/build/release/modb

# Runtime: usuário não privilegiado, sem toolchain, rootfs montável read-only.
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home --home-dir /home/modb modb \
    && mkdir -p /data \
    && chown modb:modb /data
COPY --from=build /src/build/release/modb /usr/local/bin/modb
COPY deploy/entrypoint.sh /usr/local/bin/modb-entrypoint
RUN chmod 755 /usr/local/bin/modb /usr/local/bin/modb-entrypoint
USER modb
ENV MODB_DB_PATH=/data/db.modb \
    MODB_HOST=0.0.0.0 \
    MODB_PORT=7400 \
    MODB_READY_FILE=/tmp/modb-ready \
    MODB_LIVE_FILE=/tmp/modb-live
EXPOSE 7400
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/modb-entrypoint"]
