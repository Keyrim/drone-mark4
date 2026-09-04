# =============================================================================
# drone-mark4 - development image
#
# Ships the three toolchains (native gcc, arm-none-eabi, ESP-IDF), the code quality tools
# (clang-format/clang-tidy pinned via apt.llvm.org), the debug tools
# (gdb-multiarch, J-Link), Flutter + the Android SDK for the mobile app,
# plus gh and Claude Code.
# Also serves as the CI image (pushed to GHCR): a single source of truth.
# =============================================================================

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Pinned versions (a single line to change for an upgrade)
ARG ARM_TOOLCHAIN_VERSION=14.2.rel1
ARG LLVM_VERSION=21
ARG JLINK_VERSION=V812
ARG GODOT_VERSION=4.7.1-stable
ARG NODE_VERSION=22.21.1
ARG PNPM_VERSION=11.21.0
ARG IDF_VERSION=v6.0.2
ARG FLUTTER_VERSION=3.44.8
ARG ANDROID_CMDLINE_TOOLS=11076708
# Flutter 3.44.8 builds against compileSdk 36 and its AGP 9 resolves the NDK
# at Gradle configuration time even without native code, so the NDK is part
# of the image. android-34 is the compileSdk of the usual plugins. The SDK
# cmake is the one Gradle drives for externalNativeBuild (the C++ transport
# compiled into the app).
ARG ANDROID_PLATFORM=android-36
ARG ANDROID_BUILD_TOOLS=36.0.0
ARG ANDROID_NDK=28.2.13676358
ARG ANDROID_CMAKE=3.22.1
# Chips the ESP-IDF toolchains are installed for (comma separated); the board
# is an ESP32-C3 SuperMini. Adding a chip later is this line and nothing else.
ARG IDF_TARGETS=esp32c3

# -----------------------------------------------------------------------------
# Layer 1 - base tools, native compiler, build system, debug, Python
#   - distro gcc for the desktop presets (no clang for compiling)
#   - git is not here: it comes from the git-core PPA in layer 1b
#   - Ubuntu 24.04 cmake 3.28 (>= 3.25 required) + Ninja
#   - gdb-multiarch for cross debugging via JLinkGDBServer (both run in the
#     container, see layer 4)
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        wget \
        unzip \
        xz-utils \
        gnupg \
        pkg-config \
        sudo \
        build-essential \
        gdb-multiarch \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        python3-venv \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Layer 1b - git from the git-core PPA (>= 2.48 for relative worktree paths)
#   noble ships git 2.43, which predates `worktree.useRelativePaths` (git
#   2.48). With it the links between a checkout under worktrees/ and the
#   main clone are relative, so they resolve both at the container path
#   (/workspaces/drone-mark4) and at the host's own clone path, which is
#   different (see worktrees/README.md). Same keyring + sources.list.d
#   pattern as the gh layer, no software-properties-common. The key is the
#   4096-bit one Launchpad lists on
#   https://launchpad.net/~git-core/+archive/ubuntu/ppa; the 1024-bit key
#   of 2009 still co-signs the repository but apt on noble flags it as weak.
#   The setting is written to the SYSTEM config (/etc/gitconfig) on purpose:
#   VS Code copies the host ~/.gitconfig into the container only when no
#   ~/.gitconfig exists yet, and a --global write would create one and
#   silently suppress that copy, leaving the container without user.name /
#   user.email. Here rather than in postCreateCommand: the image is the
#   single source of truth for the environment.
# -----------------------------------------------------------------------------
ARG GIT_PPA_KEY=F911AB184317630C59970973E363C90F8F1B6217
RUN curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x${GIT_PPA_KEY}" \
        | gpg --dearmor -o /usr/share/keyrings/git-core-ppa-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/git-core-ppa-keyring.gpg] https://ppa.launchpadcontent.net/git-core/ppa/ubuntu noble main" \
        > /etc/apt/sources.list.d/git-core-ppa.list \
    && apt-get update && apt-get install -y --no-install-recommends git \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system worktree.useRelativePaths true \
    && git --version

# The wire codecs are generated at build time from mark4.proto: nanopb's
# generator needs the protobuf runtime and drives the protoc bundled in
# grpcio-tools (no system protoc). System-wide so the dev user and the root
# user CI jobs run as share one install.
RUN pip install --no-cache-dir --break-system-packages protobuf grpcio-tools

# -----------------------------------------------------------------------------
# Layer 2 - clang-format / clang-tidy / clangd from apt.llvm.org (pinned)
#   Exposed without a version suffix via update-alternatives. libclang-dev
#   is for ffigen (the dart:ffi binding of the C++ transport in the mobile
#   app is generated, never written by hand); ffigen finds it under
#   /usr/lib/llvm-${LLVM_VERSION}.
# -----------------------------------------------------------------------------
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-format-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} \
        clangd-${LLVM_VERSION} \
        libclang-${LLVM_VERSION}-dev \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/clang-tidy   clang-tidy   /usr/bin/clang-tidy-${LLVM_VERSION}   100 \
    && update-alternatives --install /usr/bin/clangd       clangd       /usr/bin/clangd-${LLVM_VERSION}       100 \
    # Safety net: direct symlink for any suffixed binary of the package that
    # does not have its short name yet (run-clang-tidy, clang-apply-replacements...)
    && for tool in /usr/bin/*-${LLVM_VERSION}; do \
           short="${tool%-${LLVM_VERSION}}"; \
           [ -e "${short}" ] || ln -s "${tool}" "${short}"; \
       done \
    && clang-format --version && clang-tidy --version && clangd --version

# -----------------------------------------------------------------------------
# Layer 3 - official ARM toolchain (developer.arm.com tarball, not the Ubuntu
#   package), installed under /opt, exposed via PATH.
# -----------------------------------------------------------------------------
RUN wget -qO /tmp/arm-gnu-toolchain.tar.xz \
        "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi.tar.xz" \
    && mkdir -p /opt/arm-gnu-toolchain \
    && tar -xJf /tmp/arm-gnu-toolchain.tar.xz --strip-components=1 -C /opt/arm-gnu-toolchain \
    && rm /tmp/arm-gnu-toolchain.tar.xz

ENV PATH="/opt/arm-gnu-toolchain/bin:${PATH}"

# -----------------------------------------------------------------------------
# Layer 4 - J-Link tools (Segger)
#   The download implies acceptance of the Segger license (--post-data):
#   documented in the README. The .deb is extracted without running its
#   install scripts: no udev daemon runs in a container, device access comes
#   from docker-compose.yml (privileged + /dev bind mount). The probe is
#   attached to WSL with usbipd; the whole stack (JLinkExe, JLinkGDBServer,
#   JLinkRTTClient) runs inside the container (see docs/bring-up.md).
# -----------------------------------------------------------------------------
RUN wget -q --post-data "accept_license_agreement=accepted" -O /tmp/jlink.deb \
        "https://www.segger.com/downloads/jlink/JLink_Linux_${JLINK_VERSION}_x86_64.deb" \
    && dpkg-deb -x /tmp/jlink.deb / \
    && rm /tmp/jlink.deb \
    && ln -sfn "/opt/SEGGER/JLink_${JLINK_VERSION}" /opt/SEGGER/JLink

ENV PATH="/opt/SEGGER/JLink:${PATH}"

# -----------------------------------------------------------------------------
# Layer 4b - Godot (official build, pinned)
#   Runs the simulator inside the container: headless for the Monte Carlo
#   batches, windowed through WSLg/X11 for interactive sessions. The editor
#   on the host remains an option, both point at the same sim-godot/ project.
# -----------------------------------------------------------------------------
# X11 client libraries Godot dlopens for the windowed mode (WSLg), plus
# fontconfig, xkbcommon and the Mesa OpenGL / EGL stack: the container exposes
# no Vulkan surface, so Godot renders through OpenGL on llvmpipe
# (libgl1-mesa-dri). libwayland-client0, libdbus-1-3 and libasound2t64 only
# silence the Wayland, D-Bus and audio fallbacks.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libxcursor1 \
        libxinerama1 \
        libxrandr2 \
        libxi6 \
        libxrender1 \
        libfontconfig1 \
        libxkbcommon0 \
        libgl1 \
        libegl1 \
        libgles2 \
        libgl1-mesa-dri \
        libwayland-client0 \
        libdbus-1-3 \
        libasound2t64 \
    && rm -rf /var/lib/apt/lists/*
RUN wget -q -O /tmp/godot.zip \
        "https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/Godot_v${GODOT_VERSION}_linux.x86_64.zip" \
    && unzip -q /tmp/godot.zip -d /opt/godot \
    && rm /tmp/godot.zip \
    && ln -s "/opt/godot/Godot_v${GODOT_VERSION}_linux.x86_64" /usr/local/bin/godot

# -----------------------------------------------------------------------------
# Layer 5 - GitHub CLI (official apt repository)
# -----------------------------------------------------------------------------
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        -o /usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y --no-install-recommends gh \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Layer 6 - Node.js + pnpm for the hub web pages (software/hub/pages)
#   Official tarball into /usr/local, pnpm pinned through corepack. The
#   corepack store lives in a world-readable location so the same install
#   serves both the dev user and the root user CI jobs run as.
# -----------------------------------------------------------------------------
ENV COREPACK_HOME=/usr/local/share/corepack
RUN wget -qO /tmp/node.tar.xz \
        "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    && tar -xJf /tmp/node.tar.xz --strip-components=1 -C /usr/local \
    && rm /tmp/node.tar.xz \
    && corepack enable \
    && corepack install -g pnpm@${PNPM_VERSION} \
    && node --version && pnpm --version

# -----------------------------------------------------------------------------
# Layer 6b - ESP-IDF for the ESP32 bridge (esp32-bridge/)
#   Shallow clone of the pinned release into /opt/esp-idf, toolchains into
#   /opt/esp-tools. Both live under /opt and are made world readable so the
#   same install serves the dev user and the root user CI jobs run as (same
#   reason as COREPACK_HOME above).
#   The download cache (${IDF_TOOLS_PATH}/dist, tarballs already unpacked) is
#   deleted right after install to keep the image lean. openocd-esp32 stays,
#   although the bridge is flashed and monitored over USB serial and never
#   over JTAG: it is a 2.6 MB download next to the 2.8 GB riscv32-esp-elf
#   toolchain, and tools.json marks it install=always, so export.sh refuses
#   to activate the environment when it is missing.
#   idf.py is wrapped in /usr/local/bin so it works in a fresh shell with no
#   "source export.sh" step. The build runs git describe against /opt/esp-idf
#   and against its submodules, all root owned, so every one of those paths is
#   whitelisted in the system git config or a non-root build fails on dubious
#   ownership. Enumerating the submodules is not paranoia: safe.directory
#   takes no path prefix, only exact paths (or a bare * for everything).
# -----------------------------------------------------------------------------
ENV IDF_PATH=/opt/esp-idf
ENV IDF_TOOLS_PATH=/opt/esp-tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        flex \
        bison \
        gperf \
        libffi-dev \
        libssl-dev \
        libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --recursive --shallow-submodules \
        --branch "${IDF_VERSION}" https://github.com/espressif/esp-idf "${IDF_PATH}" \
    && git config --system --add safe.directory "${IDF_PATH}" \
    && git -C "${IDF_PATH}" submodule foreach --recursive --quiet \
        'git config --system --add safe.directory "${PWD}"' \
    && "${IDF_PATH}/install.sh" "${IDF_TARGETS}" \
    && rm -rf "${IDF_TOOLS_PATH}/dist" \
    && chmod -R a+rX "${IDF_PATH}" "${IDF_TOOLS_PATH}"

# Sourcing export.sh on every call costs a second but keeps a single source of
# truth for the environment (PATH, python venv, tool versions).
RUN printf '%s\n' \
        '#!/usr/bin/env bash' \
        '# Wrapper: enter the ESP-IDF environment, then run the real idf.py.' \
        'set -e' \
        '. "${IDF_PATH}/export.sh" > /dev/null' \
        'exec "${IDF_PATH}/tools/idf.py" "$@"' \
        > /usr/local/bin/idf.py \
    && chmod 0755 /usr/local/bin/idf.py

# -----------------------------------------------------------------------------
# Layer 6c - Android SDK for the mobile app (software/mobile)
#   Command line tools + platform-tools (adb) + platforms + build-tools + NDK
#   + cmake under /opt/android-sdk. The tree is handed to the dev user in
#   layer 7 so Gradle can install a component a plugin asks for instead of
#   failing on a read-only SDK; CI runs as root and can write anyway.
# -----------------------------------------------------------------------------
ENV ANDROID_HOME=/opt/android-sdk
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk-headless \
        zip \
    && rm -rf /var/lib/apt/lists/*
RUN mkdir -p "${ANDROID_HOME}/cmdline-tools" \
    && cd "${ANDROID_HOME}/cmdline-tools" \
    && curl -sSLo tools.zip "https://dl.google.com/android/repository/commandlinetools-linux-${ANDROID_CMDLINE_TOOLS}_latest.zip" \
    && unzip -q tools.zip && mv cmdline-tools latest && rm tools.zip \
    && yes | "${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager" --licenses > /dev/null \
    && "${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager" \
        "platform-tools" \
        "platforms;${ANDROID_PLATFORM}" \
        "platforms;android-34" \
        "build-tools;${ANDROID_BUILD_TOOLS}" \
        "ndk;${ANDROID_NDK}" \
        "cmake;${ANDROID_CMAKE}"
ENV PATH="${ANDROID_HOME}/platform-tools:${PATH}"
# adb discovers phones in wireless debugging through its built-in mDNS
# backend (no avahi in the container); host networking carries the
# announcements in. See scripts/adb_wifi.sh.
ENV ADB_MDNS_OPENSCREEN=1

# -----------------------------------------------------------------------------
# Layer 7 - non-root user with NOPASSWD sudo
#   Ubuntu 24.04 ships an "ubuntu" user (UID 1000): replace it. The
#   devcontainer extension remaps the UID/GID onto the host's at first start
#   (updateRemoteUserUID).
# -----------------------------------------------------------------------------
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

RUN userdel -r ubuntu || true \
    && groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} \
    && usermod -aG dialout,plugdev ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME} \
    && chown -R ${USERNAME}:${USERNAME} "${ANDROID_HOME}"

# -----------------------------------------------------------------------------
# Layer 8 - persisted gh and Claude Code configuration
#   These two directories are mounted as named Docker volumes (see
#   devcontainer.json); the environment variables point the tools at them so
#   logins and settings survive image rebuilds.
# -----------------------------------------------------------------------------
ENV GH_CONFIG_DIR=/home/${USERNAME}/.config/gh
ENV CLAUDE_CONFIG_DIR=/home/${USERNAME}/.claude

USER ${USERNAME}
WORKDIR /home/${USERNAME}

RUN mkdir -p "${GH_CONFIG_DIR}" "${CLAUDE_CONFIG_DIR}"

# -----------------------------------------------------------------------------
# Layer 8b - Flutter SDK (official stable tarball, pinned), owned by dev
#   Flutter is installed by the user who runs it: it writes into its own tree
#   (artifact cache, version stamp) and refuses to be told otherwise. The
#   Android artifacts are fetched now so the first build is offline-safe.
#   CI runs as root inside the same image: root writes anywhere, and the
#   safe.directory entry keeps git from rejecting the dev-owned checkout
#   (flutter shells out to git in its own tree to know its version).
#   Gradle, pub and adb keys live in the named volumes of docker-compose.yml
#   (the directories exist here so the volumes inherit the dev ownership).
# -----------------------------------------------------------------------------
RUN curl -sSL "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_${FLUTTER_VERSION}-stable.tar.xz" \
        | tar -xJ -C "/home/${USERNAME}" \
    && sudo git config --system --add safe.directory "/home/${USERNAME}/flutter" \
    && mkdir -p "/home/${USERNAME}/.gradle" "/home/${USERNAME}/.pub-cache" "/home/${USERNAME}/.android"
ENV PATH="/home/${USERNAME}/flutter/bin:${PATH}"
RUN flutter --disable-analytics > /dev/null \
    && flutter config --no-cli-animations > /dev/null \
    && flutter precache --android \
    && flutter --version

# gdb: debuginfod symbol downloads stall behind restricted networks and
# freeze every debug session at startup (the inferior never gets resumed).
# Disabling is not enough: the VS Code cpptools debug engine (MIEngine)
# unconditionally sends "set debuginfod enabled on" after user setup
# commands, so the URL list is emptied as well - with no server configured
# the feature is inert no matter who turns it back on.
RUN printf 'set debuginfod enabled off\nset debuginfod urls ""\n' > "/home/${USERNAME}/.gdbinit"

# -----------------------------------------------------------------------------
# Layer 9 - Claude Code (native installer, binary in ~/.local/bin)
# -----------------------------------------------------------------------------
RUN curl -fsSL https://claude.ai/install.sh | bash

ENV PATH="/home/${USERNAME}/.local/bin:${PATH}"
