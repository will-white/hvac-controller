# Build/dev image for the HVAC controller firmware.
# Used by BOTH the VS Code devcontainer and scripts/docker-build.sh, so CLI
# builds and the IDE share one toolchain image (and one platformio-cache volume).
FROM mcr.microsoft.com/devcontainers/base:bookworm

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    python3 \
    python3-pip \
    python3-venv \
    git \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# PlatformIO in its own venv (bookworm's system Python is PEP 668
# externally-managed, so a bare `pip install` would be refused).
RUN python3 -m venv /opt/pio \
    && /opt/pio/bin/pip install --no-cache-dir --upgrade platformio \
    && ln -s /opt/pio/bin/pio /usr/local/bin/pio \
    && ln -s /opt/pio/bin/platformio /usr/local/bin/platformio

# (No udev rules here: they only matter on the host side. For flashing, pass
# the board through with `docker run --device /dev/ttyACM0 ...` or use usbipd
# on WSL and upload from the devcontainer.)

USER vscode

# Owned by vscode so the named volumes inherit the right ownership on first
# mount. Platform/toolchain downloads (the pioarduino espressif32 fork with
# ESP32-C6 support, per platformio.ini) land in .platformio on the first build;
# .claude holds Claude Code's config/credentials (see CLAUDE_CONFIG_DIR in
# devcontainer.json). Both persist in their volumes.
#
# Claude Code itself is installed by a devcontainer feature, so it is present in
# the IDE container but NOT in the image scripts/docker-build.sh runs — features
# are a devcontainer-spec thing and a plain `docker build` skips them.
RUN mkdir -p /home/vscode/.platformio /home/vscode/.claude
