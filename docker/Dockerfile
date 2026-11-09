FROM ubuntu:20.04

ARG SMING_REPO=https://github.com/pljakobs/Sming

ARG SMING_BRANCH=develop
ARG ESP32_HARDWARE_PWM_COMMIT=f382bf29811af99c290925cc7621e678f65d6371
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

RUN git clone $SMING_REPO -b $SMING_BRANCH /opt/Sming 

WORKDIR "/opt/Sming"
# Use the ConfigDB commit pinned by pljakobs/Sming (fork tracking mikee47 feature/json-rpc,
# which provides the JsonRPC support used by the firmware)
RUN git submodule update --init Sming/Libraries/ConfigDB
RUN echo "ConfigDB pinned at: $(git -C Sming/Libraries/ConfigDB rev-parse --short HEAD)"
RUN git submodule update --init Sming/Libraries/Esp32HardwarePwm \
    && git -C Sming/Libraries/Esp32HardwarePwm fetch origin "$ESP32_HARDWARE_PWM_COMMIT" \
    && git -C Sming/Libraries/Esp32HardwarePwm checkout --detach "$ESP32_HARDWARE_PWM_COMMIT" \
    && test "$(git -C Sming/Libraries/Esp32HardwarePwm rev-parse HEAD)" = "$ESP32_HARDWARE_PWM_COMMIT"

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