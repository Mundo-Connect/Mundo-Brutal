#!/bin/sh
set -eu

MODULE_NAME="mundo"
MODULE_VERSION="0.1.0"
KREL="$(uname -r)"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
USR_SRC="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"
AUTOLOAD_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"

have()
{
    command -v "$1" >/dev/null 2>&1
}

as_root()
{
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif have sudo; then
        sudo "$@"
    else
        echo "This command needs root. Re-run as root or install sudo." >&2
        exit 1
    fi
}

install_deps()
{
    if have make && have gcc && [ -e "/lib/modules/${KREL}/build/Makefile" ]; then
        return
    fi

    if have apt-get; then
        as_root apt-get update
        as_root apt-get install -y clang make gcc dkms "linux-headers-${KREL}" build-essential || \
            as_root apt-get install -y clang make gcc dkms linux-headers-amd64 build-essential || \
            as_root apt-get install -y clang make gcc build-essential
    elif have apk; then
        flavor="${KREL##*-}"
        as_root apk update || true
        as_root apk add --no-cache clang make gcc musl-dev linux-headers dkms
        as_root apk add --no-cache "linux-${flavor}-dev" || \
            as_root apk add --no-cache linux-lts-dev || \
            as_root apk add --no-cache linux-virt-dev || true
    elif have dnf; then
        as_root dnf install -y clang make gcc dkms "kernel-devel-${KREL}" "kernel-headers-${KREL}" elfutils-libelf-devel || \
            as_root dnf install -y clang make gcc kernel-devel kernel-headers elfutils-libelf-devel
    elif have yum; then
        as_root yum install -y clang make gcc dkms "kernel-devel-${KREL}" "kernel-headers-${KREL}" elfutils-libelf-devel || \
            as_root yum install -y clang make gcc kernel-devel kernel-headers elfutils-libelf-devel
    elif have pacman; then
        as_root pacman -Sy --needed --noconfirm clang make gcc dkms linux-headers || \
            as_root pacman -Sy --needed --noconfirm clang make gcc dkms linux-lts-headers || \
            as_root pacman -Sy --needed --noconfirm clang make gcc
    elif have zypper; then
        as_root zypper --non-interactive install clang make gcc dkms kernel-devel kernel-default-devel
    elif have xbps-install; then
        as_root xbps-install -Sy clang make gcc dkms linux-headers
    else
        echo "Unknown package manager. Install clang, make, gcc, dkms and kernel headers manually." >&2
    fi
}

check_headers()
{
    if [ ! -e "/lib/modules/${KREL}/build/Makefile" ]; then
        echo "Kernel build headers for ${KREL} were not found at /lib/modules/${KREL}/build." >&2
        echo "Install headers/dev for the running kernel, not necessarily the newest kernel package." >&2
        exit 1
    fi
}

module_loaded()
{
    lsmod 2>/dev/null | awk '{print $1}' | grep -qx "${MODULE_NAME}"
}

stop_module()
{
    if module_loaded; then
        as_root modprobe -r "${MODULE_NAME}"
    fi
}

install_sources()
{
    as_root rm -rf "${USR_SRC}"
    as_root mkdir -p "${USR_SRC}"
    as_root install -m 0644 "${SCRIPT_DIR}/mundo.c" "${USR_SRC}/mundo.c"
    as_root install -m 0644 "${SCRIPT_DIR}/Makefile" "${USR_SRC}/Makefile"
    as_root install -m 0644 "${SCRIPT_DIR}/dkms.conf" "${USR_SRC}/dkms.conf"
    as_root install -m 0644 "${SCRIPT_DIR}/README.md" "${USR_SRC}/README.md"
}

dkms_remove_existing()
{
    if have dkms; then
        as_root dkms remove -m "${MODULE_NAME}" -v "${MODULE_VERSION}" --all >/dev/null 2>&1 || true
    fi
}

build_with_dkms()
{
    dkms_remove_existing
    install_sources
    as_root dkms add -m "${MODULE_NAME}" -v "${MODULE_VERSION}"
    as_root dkms build -m "${MODULE_NAME}" -v "${MODULE_VERSION}" -k "${KREL}"
    as_root dkms install -m "${MODULE_NAME}" -v "${MODULE_VERSION}" -k "${KREL}"
}

build_without_dkms()
{
    make -C "${SCRIPT_DIR}" KERNEL_RELEASE="${KREL}" clean || true
    make -C "${SCRIPT_DIR}" KERNEL_RELEASE="${KREL}"
    as_root mkdir -p "/lib/modules/${KREL}/extra"
    as_root install -m 0644 "${SCRIPT_DIR}/${MODULE_NAME}.ko" "/lib/modules/${KREL}/extra/${MODULE_NAME}.ko"
}

enable_autoload()
{
    as_root sh -c "printf '%s\n' '${MODULE_NAME}' > '${AUTOLOAD_FILE}'"
}

load_module()
{
    as_root depmod -a "${KREL}"
    if ! module_loaded; then
        as_root modprobe "${MODULE_NAME}"
    fi
}

install_module()
{
    install_deps
    check_headers
    stop_module || true

    if have dkms; then
        build_with_dkms
    else
        echo "DKMS is not available; installing only for the running kernel." >&2
        build_without_dkms
    fi

    enable_autoload
    load_module
    echo "Installed and loaded ${MODULE_NAME} for kernel ${KREL}."
}

uninstall_module()
{
    stop_module || true
    dkms_remove_existing
    as_root rm -f "${AUTOLOAD_FILE}"
    as_root rm -rf "${USR_SRC}"
    as_root rm -f "/lib/modules/${KREL}/extra/${MODULE_NAME}.ko"
    as_root depmod -a "${KREL}" || true
    echo "Uninstalled ${MODULE_NAME}."
}

case "${1:-install}" in
    install|reinstall|repair)
        install_module
        ;;
    uninstall|remove)
        uninstall_module
        ;;
    *)
        echo "Usage: sh install.sh [install|reinstall|repair|uninstall]" >&2
        exit 2
        ;;
esac
