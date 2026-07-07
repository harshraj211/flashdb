FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --config Release -j"$(nproc)"

FROM ubuntu:24.04

RUN useradd --create-home --shell /usr/sbin/nologin flashdb \
    && mkdir -p /app/data \
    && chown -R flashdb:flashdb /app

WORKDIR /app
COPY --from=build /app/build/flashdb /app/flashdb

USER flashdb

ENV PORT=6379
EXPOSE 6379

CMD ["/bin/sh", "-c", "if [ -n \"$FLASHDB_PASSWORD\" ]; then /app/flashdb --host 0.0.0.0 --port ${PORT:-6379} --aof-path /app/data/appendonly.aof --requirepass \"$FLASHDB_PASSWORD\"; else /app/flashdb --host 0.0.0.0 --port ${PORT:-6379} --aof-path /app/data/appendonly.aof; fi"]
