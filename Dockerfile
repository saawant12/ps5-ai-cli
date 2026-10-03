ARG PS5_SDK_IMAGE=ps5-ai-cli:sdk
FROM rust:1.95.0-slim-trixie AS rust

FROM ${PS5_SDK_IMAGE}
COPY --from=rust /usr/local/cargo /usr/local/cargo
COPY --from=rust /usr/local/rustup /usr/local/rustup
ENV RUSTUP_HOME=/usr/local/rustup CARGO_HOME=/usr/local/cargo
ENV PATH=/usr/local/cargo/bin:/usr/lib/llvm-19/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
RUN rustup component add rust-src && rustup target add x86_64-unknown-freebsd
COPY tools/prepare-rust-std.py /tmp/prepare-rust-std.py
RUN python3 /tmp/prepare-rust-std.py && rm /tmp/prepare-rust-std.py
WORKDIR /work
ENTRYPOINT ["bash"]
