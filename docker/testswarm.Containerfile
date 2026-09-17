# Runtime image for testswarm.sh — runs the 32-bit Sming Host emulator inside a
# container with a TAP interface + full NAT, so the emulated firmware is
# reachable from other containers (e.g. an nginx webserver) on a shared bridge.
#
# This image intentionally does NOT contain the Sming toolchain: the app is
# built on the host and bind-mounted in. It only carries the 32-bit runtime
# libraries, valgrind, and the network tooling needed to wire up the TAP + NAT.
FROM fedora:42

# 32-bit runtime libs for the i386 Host binary (libstdc++/libm/libgcc/libc),
# the sanitizer runtimes for `asan` mode, valgrind for the memcheck/dhat/massif
# modes, and iproute2/iptables/nftables for the in-container TAP + NAT setup.
RUN dnf -y install --setopt=install_weak_deps=False \
        glibc.i686 \
        libstdc++.i686 \
        libgcc.i686 \
        libasan.i686 \
        libubsan.i686 \
        valgrind \
        iproute \
        iptables-nft \
        nftables \
        procps-ng \
        bash \
    && dnf clean all

ENTRYPOINT ["/usr/local/bin/testswarm-run.sh"]
