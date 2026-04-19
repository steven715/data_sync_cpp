FROM ubuntu:24.04

# 確保 non-interactive 模式，避免安裝過程卡住
ENV DEBIAN_FRONTEND=noninteractive

# 更新並安裝必要工具
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
# 建置工具鏈
        build-essential \
        cmake \
        pkg-config \
        zip \
        unzip \
# 版本控制
        git \
# 程式碼品質 / 靜態分析
        clang-format \
        cppcheck \
# 偵錯
        gdb \
        strace \
# 效能分析 / profiling
        linux-tools-generic \
        google-perftools \
        libgoogle-perftools-dev \
        heaptrack \
        bpftrace \
# 編輯器與系統監控
        vim \
        htop \
# 網路診斷與 TLS 信任
        ca-certificates \
        curl \
        iproute2 \
        iputils-ping && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# FlameGraph（perf 火焰圖產生器，runbook 2.5 使用）
RUN git clone --depth=1 https://github.com/brendangregg/FlameGraph.git /opt/FlameGraph
ENV PATH="/opt/FlameGraph:${PATH}"

# 安裝 vcpkg
# Ubuntu 24.04 的 glibc 與 vcpkg 預編譯 binary 不相容，
# 強制走系統工具 fallback 自行編譯 vcpkg
WORKDIR /opt
RUN git clone https://github.com/microsoft/vcpkg.git && \
    cd vcpkg && \
    VCPKG_FORCE_SYSTEM_BINARIES=1 ./bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# 設定環境變數（方便後續使用）
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"