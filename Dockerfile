FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config \
    libssl-dev libevent-dev libboost-all-dev libdb5.3++-dev \
    libzmq3-dev libgmp-dev libsodium-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain stable --profile minimal
ENV PATH="/root/.cargo/bin:${PATH}"

WORKDIR /build/hemisd
COPY . /build/hemisd/

RUN ./autogen.sh && \
    ./configure \
        --without-gui \
        --disable-tests \
        --disable-bench \
        --with-incompatible-bdb \
        --without-miniupnpc \
        --prefix=/usr/local && \
    make -j$(nproc) && \
    make install && \
    strip /usr/local/bin/Hemisd /usr/local/bin/Hemis-cli
