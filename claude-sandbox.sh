#!/bin/bash

set -euo pipefail

# Claude Code Sandbox Script
# Run this from any project directory to launch Claude in a sandboxed environment

# Dependency checks
if ! command -v bwrap &>/dev/null; then
    echo "bwrap is not installed. Install bubblewrap and try again." >&2
    exit 1
fi
if ! command -v claude &>/dev/null; then
    echo "claude is not installed or not in PATH." >&2
    exit 1
fi

PROJECT_DIR="$(pwd)"
CLAUDE_PATH="$(command -v claude)"
CLAUDE_REAL_PATH="$(readlink -f "$CLAUDE_PATH")"
CLAUDE_BIN_DIR="$(dirname "$CLAUDE_PATH")"
CLAUDE_INSTALL_DIR="$(dirname "$(dirname "$CLAUDE_REAL_PATH")")"

# Special arguments
noproject=false
ssh_forward=false
pg_forward=false
kube_forward=false
#flag=
# we rely upon bwrap here
flag=--dangerously-skip-permissions
COMMAND="$CLAUDE_PATH"
while true; do
    case "${1:-}" in
        --noproject)
            noproject=true
            shift
            ;;
        --shell)
            COMMAND=/bin/bash
            flag=
            shift
            ;;
        --ssh)
            ssh_forward=true
            shift
            ;;
        --pg)
            pg_forward=true
            shift
            ;;
        --kube)
            kube_forward=true
            shift
            ;;
        *)
            break
            ;;
    esac
done


# Sanity check
if [[ "$PROJECT_DIR" == "$HOME" ]]; then
    echo "Refusing to sandbox the whole home" >&2
    exit 1
fi
if [[ "$noproject" == false ]] && [[ ! -e ".git" ]] && [[ ! -e ".jj" ]]; then
    echo "We only sandbox project by default. Override with --noproject if needed." >&2
    exit 1
fi

# Ensure directories exist
mkdir -p "$HOME/.claude"
mkdir -p "$HOME/.cache/claude-cli-nodejs"
mkdir -p "$HOME/.cache/claude"
mkdir -p "$HOME/.local/state/claude"
mkdir -p "$HOME/agent"

BWRAP_ARGS=(
    # System binaries and libraries (read-only)
    --ro-bind /usr /usr
    --ro-bind /lib /lib
    --ro-bind /lib64 /lib64
    --ro-bind /bin /bin

    # System config (read-only)
    --ro-bind /etc/resolv.conf /etc/resolv.conf
    --ro-bind /etc/hosts /etc/hosts
    --ro-bind /etc/nsswitch.conf /etc/nsswitch.conf
    --ro-bind /etc/ssl /etc/ssl
    --ro-bind /etc/passwd /etc/passwd
    --ro-bind /etc/group /etc/group
    --ro-bind /etc/alternatives /etc/alternatives
    --ro-bind /etc/fonts /etc/fonts

    --ro-bind /etc/apt /etc/apt
    --ro-bind /var/lib/apt /var/lib/apt
    --ro-bind /var/lib/dpkg /var/lib/dpkg

    # Separate (but persistent home)
    --bind "$HOME/agent" "$HOME"

    # Claude installation (read-only)
    --ro-bind "$CLAUDE_BIN_DIR" "$CLAUDE_BIN_DIR"
    --ro-bind "$CLAUDE_INSTALL_DIR" "$CLAUDE_INSTALL_DIR"

    # Claude config and state (read-write for auth/settings)
    --bind "$HOME/.claude" "$HOME/.claude"
    --bind "$HOME/.cache/claude-cli-nodejs" "$HOME/.cache/claude-cli-nodejs"
    --bind "$HOME/.cache/claude" "$HOME/.cache/claude"
    --bind "$HOME/.local/state/claude" "$HOME/.local/state/claude"

    # Project directory (read-write)
    --bind "$PROJECT_DIR" "$PROJECT_DIR"

    # Temp, proc, dev
    --tmpfs /tmp
    --proc /proc
    --dev /dev
    --ro-bind /sys /sys

    # Temp dir for claude
    --dir /tmp/claude-1000

    # Strip display/IPC env so the agent cannot reach the host X server,
    # Wayland compositor, or session D-Bus via the shared network namespace.
    --unsetenv DISPLAY
    --unsetenv XAUTHORITY
    --unsetenv WAYLAND_DISPLAY
    --unsetenv DBUS_SESSION_BUS_ADDRESS
    --unsetenv XDG_RUNTIME_DIR

    # Namespacing (--share-net is the default; variants like the networking
    # wrapper override this by appending --unshare-net later)
    --unshare-pid
    --die-with-parent
    --chdir "$PROJECT_DIR"
)

# Bind /dev/kvm for accelerated QEMU, if present. NOTE: bwrap's unprivileged
# user namespace maps only your own uid/gid, so your host `kvm` group membership
# collapses to the overflow gid (nogroup) inside the sandbox. A 0660 root:kvm
# device therefore stays unreadable in here regardless of this bind. For the
# sandbox to actually use it, the node must be world-rw on the host, e.g.:
#   echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666"' | sudo tee /etc/udev/rules.d/65-kvm.rules
#   sudo udevadm control --reload-rules && sudo udevadm trigger /dev/kvm
if [[ -e /dev/kvm ]]; then
    BWRAP_ARGS+=(--dev-bind-try /dev/kvm /dev/kvm)
    if [[ ! ( -r /dev/kvm && -w /dev/kvm ) ]]; then
        echo "warning: /dev/kvm is not world rw; KVM acceleration will be unavailable inside the sandbox (see note in $0)." >&2
    fi
fi

# Protect .git/hooks and .git/config from modification (read-only overlays on top of the
# rw project bind). .git/config matters because core.hooksPath there can redirect hook
# lookup to an arbitrary writable path, bypassing the hooks overlay.
if [[ -d "$PROJECT_DIR/.git/hooks" ]]; then
    BWRAP_ARGS+=(--ro-bind "$PROJECT_DIR/.git/hooks" "$PROJECT_DIR/.git/hooks")
fi
if [[ -f "$PROJECT_DIR/.git/config" ]]; then
    BWRAP_ARGS+=(--ro-bind "$PROJECT_DIR/.git/config" "$PROJECT_DIR/.git/config")
fi
if [[ -f "$PROJECT_DIR/.git/config.worktree" ]]; then
    BWRAP_ARGS+=(--ro-bind "$PROJECT_DIR/.git/config.worktree" "$PROJECT_DIR/.git/config.worktree")
fi

# Add /etc/localtime if it exists (for correct timezone)
if [[ -f /etc/localtime ]]; then
    BWRAP_ARGS+=(--ro-bind /etc/localtime /etc/localtime)
fi

# Add ~/.claude.json if it exists
if [[ -f "$HOME/.claude.json" ]]; then
    BWRAP_ARGS+=(--bind "$HOME/.claude.json" "$HOME/.claude.json")
fi

# Add .gitconfig if it exists
if [[ -f "$HOME/.gitconfig" ]]; then
    BWRAP_ARGS+=(--ro-bind "$HOME/.gitconfig" "$HOME/.gitconfig")
fi

# Add .nvm if it exists (needed for node)
if [[ -d "$HOME/.nvm" ]]; then
    BWRAP_ARGS+=(--ro-bind "$HOME/.nvm" "$HOME/.nvm")
fi

# Add local typst if it exists
if [[ -d "$HOME/.local/share/typst" ]]; then
    BWRAP_ARGS+=(--ro-bind "$HOME/.local/share/typst" "$HOME/.local/share/typst")
fi

# Add local texmf if it exists
if [[ -d "/var/lib/texmf" ]]; then
    BWRAP_ARGS+=(--ro-bind "/var/lib/texmf" "/var/lib/texmf")
fi


# Add Rust toolchain if it exists (needed for cargo)
# dont, the agent gets its own
#if [[ -d "$HOME/.cargo" ]]; then
#    BWRAP_ARGS+=(--ro-bind "$HOME/.cargo" "$HOME/.cargo")
#fi
#
#if [[ -d "$HOME/.rustup" ]]; then
#    BWRAP_ARGS+=(--ro-bind "$HOME/.rustup" "$HOME/.rustup")
#fi

# Add SSH known_hosts for host verification (read-only)
if [[ -f "$HOME/.ssh/known_hosts" ]]; then
    BWRAP_ARGS+=(--ro-bind "$HOME/.ssh/known_hosts" "$HOME/.ssh/known_hosts")
fi

# Postgres forwarding (only when --pg is pased)
if [[ "$pg_forward" == true ]]; then
    if [[ -S "/var/run/postgresql/.s.PGSQL.5432" ]]; then
        BWRAP_ARGS+=(--bind "/var/run/postgresql/.s.PGSQL.5432" "/var/run/postgresql/.s.PGSQL.5432")
    fi
    if [[ -S "/tmp/.s.PGSQL.5439" ]]; then
        BWRAP_ARGS+=(--bind "/tmp/.s.PGSQL.5439" "/tmp/.s.PGSQL.5439")
    fi
fi

# Kubernetes config (only when --kube is passed)
if [[ "$kube_forward" == true ]]; then
    if [[ -d "$HOME/.kube" ]]; then
        BWRAP_ARGS+=(--bind "$HOME/.kube" "$HOME/.kube")
    fi
    if [[ -d "$HOME/.minikube" ]]; then
        BWRAP_ARGS+=(--ro-bind "$HOME/.minikube" "$HOME/.minikube")
    fi
fi

# SSH agent forwarding (only when --ssh is passed)
if [[ "$ssh_forward" == true ]]; then
    if [[ -n "${SSH_AUTH_SOCK:-}" ]] && [[ -S "$SSH_AUTH_SOCK" ]]; then
        BWRAP_ARGS+=(--bind "$SSH_AUTH_SOCK" "$SSH_AUTH_SOCK")
        BWRAP_ARGS+=(--setenv SSH_AUTH_SOCK "$SSH_AUTH_SOCK")
    fi
    if [[ -d "$HOME/.ssh" ]]; then
        BWRAP_ARGS+=(--ro-bind "$HOME/.ssh" "$HOME/.ssh")
    fi
fi

# Hand off: a variant (e.g. claude-sandbox-networking.sh) may source this
# script and define `sandbox_run` to wrap bwrap with extra setup/cleanup.
# Otherwise just exec bwrap directly.
if declare -F sandbox_run >/dev/null; then
    sandbox_run "$@"
else
    exec bwrap "${BWRAP_ARGS[@]}" -- "$COMMAND" ${flag:+$flag} "$@"
fi
