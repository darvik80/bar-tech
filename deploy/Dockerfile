FROM rust:1.85-bookworm AS builder
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake pkg-config libopus-dev \
    && rm -rf /var/lib/apt/lists/*
COPY Cargo.toml Cargo.lock ./
COPY crates ./crates
ENV OPUS_BUNDLED=1
RUN cargo build --release -p bar-core

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libopus0 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /src/target/release/bar-core /usr/local/bin/bar-core
COPY config.example.yaml /app/config.example.yaml
COPY data/knowledge /app/data/knowledge
EXPOSE 8091 50004/udp
ENTRYPOINT ["bar-core", "/app/config.yaml"]
