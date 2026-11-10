FROM ubuntu:20.04

ARG SMING_REPO=https://github.com/pljakobs/Sming

ARG SMING_BRANCH=develop
ARG SMING_COMMIT=
ARG INSTALL_ARGS="Esp32 Esp8266"

# ------------------------------------------------------------------------------
# Pre-requisites
# ------------------------------------------------------------------------------

RUN apt-get -y update \
    && DEBIAN_FRONTEND=noninteractive \
    TZ=Europe/Berlin \
    apt-get install -y \
    git \
    sudo \
    tzdata \
    build-essential \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    python3-future \
    cmake \
    python3.8-venv

# ------------------------------------------------------------------------------
# Fetch Sming and install tools
# ------------------------------------------------------------------------------

RUN git clone "$SMING_REPO" -b "$SMING_BRANCH" /opt/Sming \
    && if [ -n "$SMING_COMMIT" ]; then \
        git -C /opt/Sming checkout --detach "$SMING_COMMIT" \
        && test "$(git -C /opt/Sming rev-parse HEAD)" = "$SMING_COMMIT"; \
    fi

WORKDIR "/opt/Sming"
# Use the ConfigDB commit pinned by pljakobs/Sming (fork tracking mikee47 feature/json-rpc,
# which provides the JsonRPC support used by the firmware)
RUN git submodule update --init Sming/Libraries/ConfigDB
RUN echo "ConfigDB pinned at: $(git -C Sming/Libraries/ConfigDB rev-parse --short HEAD)"
RUN git submodule update --init Sming/Libraries/Esp32HardwarePwm \
    && test "$(git -C Sming/Libraries/Esp32HardwarePwm rev-parse HEAD)" = \
        "$(git rev-parse HEAD:Sming/Libraries/Esp32HardwarePwm)"

SHELL ["/bin/bash", "-c"]
WORKDIR "/opt/Sming"
RUN Tools/install.sh $INSTALL_ARGS 
RUN echo "source /opt/Sming/Tools/export.sh" >> ~/.bashrc 
RUN cat  "/opt/Sming/Tools/export.sh"

WORKDIR "/opt/Sming/Sming/"
RUN make python-requirements && \
    make submodules
WORKDIR "/opt/Sming/samples/Basic_WiFi"
RUN source ~/.bashrc && \
    source /opt/Sming/Tools/export.sh && \
    make SMING_SOC=esp8266
RUN source ~/.bashrc && \
    source /opt/Sming/Tools/export.sh && \
    make SMING_SOC=esp32
RUN source ~/.bashrc && \
    source /opt/Sming/Tools/export.sh && \
    make SMING_SOC=esp32c3

WORKDIR /