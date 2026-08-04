# =============================================================================
# drone-mark4 - development image
#
# Ships both toolchains (native gcc + arm-none-eabi), the code quality tools
# (clang-format/clang-tidy pinned via apt.llvm.org), the debug tools
# (gdb-multiarch, J-Link), plus gh and Claude Code.
# Also serves as the CI image (pushed to GHCR): a single source of truth.
# =============================================================================

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Pinned versions (a single line to change for an upgrade)
ARG ARM_TOOLCHAIN_VERSION=14.2.rel1
ARG LLVM_VERSION=21
ARG JLINK_VERSION=V812

# -----------------------------------------------------------------------------
# Layer 1 - base tools, native compiler, build system, debug, Python
#   - distro gcc for the desktop presets (no clang for compiling)
#   - Ubuntu 24.04 cmake 3.28 (>= 3.25 required) + Ninja
#   - gdb-multiarch for cross debugging via JLinkGDBServer (runs on the host)
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        wget \
        unzip \
        xz-utils \
        gnupg \
        git \
        pkg-config \
        sudo \
        build-essential \
        gdb-multiarch \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        python3-venv \
        pipenv \
    && rm -rf /var/lib/apt/lists/*

# pipenv creates each tool's venv in-project (<tool>/.venv, gitignored), where
# the VS Code launch configurations expect to find the interpreter.
ENV PIPENV_VENV_IN_PROJECT=1

# -----------------------------------------------------------------------------
# Layer 2 - clang-format / clang-tidy / clangd from apt.llvm.org (pinned)
#   Exposed without a version suffix via update-alternatives.
# -----------------------------------------------------------------------------
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-format-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} \
        clangd-${LLVM_VERSION} \
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
#   install scripts (udev rules are useless in a container - the probe lives
#   on the host, where JLinkGDBServer also runs; what matters here are the
#   clients such as JLinkRTTClient).
# -----------------------------------------------------------------------------
RUN wget -q --post-data "accept_license_agreement=accepted" -O /tmp/jlink.deb \
        "https://www.segger.com/downloads/jlink/JLink_Linux_${JLINK_VERSION}_x86_64.deb" \
    && dpkg-deb -x /tmp/jlink.deb / \
    && rm /tmp/jlink.deb \
    && ln -sfn "/opt/SEGGER/JLink_${JLINK_VERSION}" /opt/SEGGER/JLink

ENV PATH="/opt/SEGGER/JLink:${PATH}"

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
# Layer 6 - Qt runtime for the ground station
#   The PyQt6 wheels bundle Qt itself but dynamically link these system
#   libraries (OpenGL, fontconfig, xcb platform plugin for X11/WSLg).
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgl1 \
        libegl1 \
        libfontconfig1 \
        libfreetype6 \
        libdbus-1-3 \
        libxkbcommon-x11-0 \
        libxcb-cursor0 \
        libxcb-icccm4 \
        libxcb-image0 \
        libxcb-keysyms1 \
        libxcb-randr0 \
        libxcb-render-util0 \
        libxcb-shape0 \
        libxcb-xkb1 \
    && rm -rf /var/lib/apt/lists/*

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
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME}

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
# Layer 9 - Claude Code (native installer, binary in ~/.local/bin)
# -----------------------------------------------------------------------------
RUN curl -fsSL https://claude.ai/install.sh | bash

ENV PATH="/home/${USERNAME}/.local/bin:${PATH}"
