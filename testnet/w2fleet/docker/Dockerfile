# ptx-w2 fleet runtime image — thin Debian + prebuilt binaries (W2.0a).
# Build binaries at HEAD via ptx-builder-local (canonical invocation), place as
# docker-w2/binaries/Hemisd-ptx-w2 and Hemis-cli-ptx-w2, then:
#   docker build -t hemis-ptx-w2:<commit> -f Dockerfile <docker-w2 dir>
# NEVER tag over hemis-ptx-bea:* — the PoC image stays untouched.

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libevent-2.1-7 \
    libevent-pthreads-2.1-7 \
    libboost-filesystem1.74.0 \
    libboost-thread1.74.0 \
    libboost-chrono1.74.0 \
    libdb5.3++ \
    libzmq5 \
    libgmp10 \
    libsodium23 \
    curl \
    && rm -rf /var/lib/apt/lists/*

COPY binaries/Hemisd-ptx-w2 /usr/local/bin/Hemisd
COPY binaries/Hemis-cli-ptx-w2 /usr/local/bin/Hemis-cli
RUN chmod +x /usr/local/bin/Hemisd /usr/local/bin/Hemis-cli

RUN mkdir -p /root/.Hemis-params
COPY params/sapling-spend.params /root/.Hemis-params/sapling-spend.params
COPY params/sapling-output.params /root/.Hemis-params/sapling-output.params

RUN mkdir -p /root/.hemis-ptxbea

EXPOSE 29994 29903

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

LABEL branch="feature/ptx-dkg" network="ptxbea" fleet="ptx-w2"

ENTRYPOINT ["/entrypoint.sh"]
