FROM ubuntu:bionic
LABEL maintainer="Màrius Montón"
ENV SYSTEMC_VERSION=2.3.3
ENV SYSTEMC_HOME=/usr/local/systemc-${SYSTEMC_VERSION}
ENV LD_LIBRARY_PATH=${SYSTEMC_HOME}/lib-linux64:${LD_LIBRARY_PATH}

# Combine apt-get commands to reduce layers
RUN apt-get update -q && apt-get install -qy --no-install-recommends \
      gcc-riscv64-linux-gnu \
      build-essential \
      curl \
      cmake \
      ninja-build \
      git \
      openssh-client \
      wget \
      g++-8 \
      xterm \
      libboost-all-dev \
      gdb \
      net-tools \
      doxygen \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
 
# Install SystemC with C++17 support
RUN mkdir -p /usr/src/riscv64 \
    && wget --no-check-certificate https://accellera.org/images/downloads/standards/systemc/systemc-${SYSTEMC_VERSION}.tar.gz \
    && tar -xzf systemc-${SYSTEMC_VERSION}.tar.gz -C /usr/src/riscv64 \
    && cd /usr/src/riscv64/systemc-${SYSTEMC_VERSION} \
    && mkdir -p build \
    && cd build \
    && cmake .. -G Ninja -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=${SYSTEMC_HOME} \
    && ninja \
    && ninja install \
    && cd / \
    && rm systemc-${SYSTEMC_VERSION}.tar.gz
 
# Configure Git and SSH
RUN mkdir -p /root/.ssh \
    && ssh-keyscan github.com > /root/.ssh/known_hosts \
    && git config --global http.sslBackend "openssl" \
    && git config --global http.sslVerify false

# Build Spdlog and RISCV-VP
RUN cd /usr/src/riscv64 \
    && chmod -R a+rw . \
    && git clone --recurse-submodules https://github.com/mariusmm/RISCV-VP.git \
    && cd RISCV-VP \
    && cd spdlog \
    && mkdir -p build _builds \
    && cd _builds \
    && cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && ninja \
    && cmake .. -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_BUILD_TYPE=Release \
    && ninja \
    && ninja install \
    && cd ../.. \
    && mkdir -p build \
    && cd build \
    && chmod a+rw . \
    && export SPDLOG_HOME=$PWD/../spdlog/install \
    && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. \
    && ninja \
    && cd / \
    && rm -rf /tmp/* /var/tmp/*

WORKDIR /usr/src/riscv64/RISCV-VP

# Set entrypoint to maintain the container running
CMD ["/bin/bash"]
