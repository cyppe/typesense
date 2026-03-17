FROM nvidia/cuda:12.8.1-cudnn-devel-ubuntu24.04

ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
ENV CUDA_HOME=/usr/local/cuda
ENV CUDNN_HOME=/usr/local/cuda

RUN apt-get update && apt-get install -y \
    ca-certificates \
    curl \
    clang-18 \
    file \
    g++-14 \
    gcc-14 \
    git \
    make \
    m4 \
    zlib1g-dev \
    lld \
    pkgconf \
    python3 \
    unzip \
    zip \
    patch \
    gawk \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 30 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 30 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 30 \
    && update-alternatives --set cc /usr/bin/gcc \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 30 \
    && update-alternatives --set c++ /usr/bin/g++ \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 30 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 30

RUN case "${TARGETARCH}" in \
      arm64) BAZELISK_URL="https://github.com/bazelbuild/bazelisk/releases/download/v1.28.1/bazelisk-linux-arm64" ;; \
      amd64|x86_64|"") BAZELISK_URL="https://github.com/bazelbuild/bazelisk/releases/download/v1.28.1/bazelisk-linux-amd64" ;; \
      *) echo "Unsupported TARGETARCH: ${TARGETARCH}"; exit 1 ;; \
    esac \
    && curl -fsSL "${BAZELISK_URL}" -o /usr/local/bin/bazel \
    && chmod +x /usr/local/bin/bazel

ENTRYPOINT ["/usr/local/bin/bazel"]
