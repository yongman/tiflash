# syntax=docker/dockerfile:1

FROM ubuntu:22.04

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]

USER root
WORKDIR /root/

ENV HOME /root/
ENV TZ Asia/Shanghai

RUN \
    --mount=type=cache,sharing=locked,target=/var/cache/apt \
    --mount=type=cache,sharing=locked,target=/var/lib/apt <<EOF

sed -i s@/archive.ubuntu.com/@/apt.ksyun.cn/@g /etc/apt/sources.list
sed -i s@/ports.ubuntu.com/@/apt.ksyun.cn/@g /etc/apt/sources.list
sed -i s@/security.ubuntu.com/@/apt.ksyun.cn/@g /etc/apt/sources.list
apt-get update -y
apt-get upgrade -y

EOF

RUN \
    --mount=type=cache,sharing=locked,target=/var/cache/apt \
    --mount=type=cache,sharing=locked,target=/var/lib/apt <<EOF

# Configure timezone
export DEBIAN_FRONTEND=noninteractive
apt-get install -y tzdata
ln -fs /usr/share/zoneinfo/$TZ /etc/localtime
dpkg-reconfigure -f noninteractive tzdata

EOF

RUN \
    --mount=type=cache,sharing=locked,target=/var/cache/apt \
    --mount=type=cache,sharing=locked,target=/var/lib/apt <<EOF

# Some debug utilities
apt-get install -y gdb strace net-tools lsof tcpdump vim

EOF

COPY cmake-build-Release/artifacts /tiflash

# Checkout files
RUN ls -la /tiflash

# Checkout version
RUN /tiflash/tiflash version

ENTRYPOINT ["/tiflash/tiflash", "server"]
