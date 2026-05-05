FROM buildpack-deps:trixie

ENV OTP_VERSION="28.4.2" \
    REBAR3_VERSION="3.26.0"

COPY . /usr/src/otp
WORKDIR /usr/src/otp
ENV ERL_TOP=/usr/src/otp

# We'll install the build dependencies for erlang-odbc along with the erlang
# build process:
RUN set -xe \
	runtimeDeps='libodbc2 \
			libsctp1 \
			libwxgtk3.2 \
			libwxgtk-webview3.2-dev  ' \
	&& buildDeps='unixodbc-dev \
			libsctp-dev ' \
	&& apt-get update \
	&& apt-get install -y --no-install-recommends $runtimeDeps \
	&& apt-get install -y --no-install-recommends $buildDeps \
	&& find . -type f \( -name config.log -o -name config.status -o -name erl_crash.dump \) -delete \
	&& find . -type d \( -name deps -o -name obj -o -name obj.debug -o -name '*-unknown-linux-gnu' \) -prune -exec rm -rf {} + \
        && ./otp_build autoconf \
        && gnuArch="$(dpkg-architecture --query DEB_HOST_GNU_TYPE)" \
        && ./configure --build="$gnuArch" \
        && make -j$(nproc) \
        && make -j$(nproc) docs DOC_TARGETS=chunks \
        && make install install-docs DOC_TARGETS=chunks \
	&& find /usr/local -name examples | xargs rm -rf \
	&& apt-get purge -y --auto-remove $buildDeps \
	&& rm -rf /var/lib/apt/lists/*

CMD ["erl"]
