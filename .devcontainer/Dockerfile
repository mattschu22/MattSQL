FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        clang-format \
        clang-tidy \
        clang-tools \
        clangd \
        cmake \
        curl \
        gdb \
        git \
        heaptrack \
        less \
        linux-tools-generic \
        lldb \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        valgrind \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* \
    && git clone --depth=1 https://github.com/brendangregg/FlameGraph /opt/flamegraph \
    && ln -s /opt/flamegraph/flamegraph.pl /usr/local/bin/flamegraph.pl \
    && ln -s /opt/flamegraph/stackcollapse-perf.pl /usr/local/bin/stackcollapse-perf.pl

ENV RUSTUP_HOME=/usr/local/rustup
ENV CARGO_HOME=/usr/local/cargo
ENV PATH=/usr/local/cargo/bin:${PATH}

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --default-toolchain stable \
    && rustup component add clippy rustfmt \
    && chown -R vscode:vscode ${RUSTUP_HOME} ${CARGO_HOME}
