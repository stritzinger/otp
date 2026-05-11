# SPDX-FileCopyrightText: 2026 Dipl.Phys. Peer Stritzinger GmbH
# SPDX-License-Identifier: Apache-2.0

FROM alpine:3.20

ENV OTP_VERSION="28.4.2" \
    REBAR3_VERSION="3.26.0"

# Build-time and run-time deps. We deliberately skip wx/odbc/jit/megaco/etc.
# to keep the build self-contained on musl.
RUN apk add --no-cache \
        bash \
        autoconf automake libtool make perl \
        gcc g++ \
        musl-dev linux-headers \
        ncurses-dev ncurses-static \
        openssl-dev openssl-libs-static \
        zlib-dev zlib-static \
        curl wget file ca-certificates git

COPY . /usr/src/otp
WORKDIR /usr/src/otp
ENV ERL_TOP=/usr/src/otp

RUN set -xe \
    && find . -type f \( -name config.log -o -name config.status -o -name erl_crash.dump \) -delete \
    && find . -type d \( -name deps -o -name obj -o -name obj.debug \
                          -o -name '*-unknown-linux-gnu' \
                          -o -name '*-unknown-linux-musl' \) -prune -exec rm -rf {} + \
    && ./otp_build autoconf \
    && ./configure \
        --without-javac \
        --without-jinterface \
        --without-wx \
        --without-megaco \
        --without-odbc \
        --without-debugger \
        --without-observer \
        --without-et \
        --disable-jit \
        --disable-dynamic-ssl-lib \
    && make -j"$(nproc)" \
    && make -j"$(nproc)" docs DOC_TARGETS=chunks \
    && make install install-docs DOC_TARGETS=chunks \
    && find /usr/local -name examples | xargs rm -rf

CMD ["erl"]

# rebar3 (kept for in-image use; calzone-sandbox installs its own copy too)
RUN set -xe \
    && REBAR3_DOWNLOAD_URL="https://github.com/erlang/rebar3/archive/${REBAR3_VERSION}.tar.gz" \
    && REBAR3_DOWNLOAD_SHA256="a151dc4a07805490e9f217a099e597ac9774814875f55da2c66545c333fdff64" \
    && mkdir -p /usr/src/rebar3-src \
    && curl -fSL -o rebar3-src.tar.gz "$REBAR3_DOWNLOAD_URL" \
    && echo "$REBAR3_DOWNLOAD_SHA256  rebar3-src.tar.gz" | sha256sum -c - \
    && tar -xzf rebar3-src.tar.gz -C /usr/src/rebar3-src --strip-components=1 \
    && rm rebar3-src.tar.gz \
    && cd /usr/src/rebar3-src \
    && HOME=$PWD ./bootstrap \
    && install -v ./rebar3 /usr/local/bin/ \
    && rm -rf /usr/src/rebar3-src
