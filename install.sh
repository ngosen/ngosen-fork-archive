#!/bin/bash
# Installs the Ngó Sen package built for this distribution from the latest GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/ngosen/ngosen/ban-dung/install.sh | bash
#
# Everything lives in functions and main runs on the last line, so a download that is cut short
# cannot run half a script.
set -euo pipefail

RELEASE_URL=${NGOSEN_RELEASE_URL:-https://github.com/ngosen/ngosen/releases/latest/download}
SOURCE_GUIDE="https://github.com/ngosen/ngosen/blob/ban-dung/KHAC-GI-SO-VOI-BAN-GOC.md#cài-sang-máy-khác"
SERVICE=fcitx5-lotus-server

assume_yes=0
manager=
pattern=
workdir=

say() { printf '%s\n' "$*"; }

die() {
    printf 'Lỗi: %s\n' "$*" >&2
    exit 1
}

unsupported() {
    die "chưa có gói dựng sẵn cho $1. Cài từ mã theo hướng dẫn: $SOURCE_GUIDE"
}

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

usage() {
    say "Cách dùng: install.sh [--yes]"
    say "  --yes   không hỏi lại trước khi cài"
}

# Sets manager and pattern; pattern is a regular expression for the package's file name.
detect_distribution() {
    [ "$(uname -m)" = x86_64 ] || unsupported "máy $(uname -m)"
    [ -r /etc/os-release ] || die "không đọc được /etc/os-release"

    local id like codename version
    # shellcheck disable=SC1091
    id=$(. /etc/os-release && printf '%s' "${ID:-}")
    # shellcheck disable=SC1091
    like=$(. /etc/os-release && printf '%s' "${ID_LIKE:-}")
    # shellcheck disable=SC1091
    codename=$(. /etc/os-release && printf '%s' "${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}")
    # shellcheck disable=SC1091
    version=$(. /etc/os-release && printf '%s' "${VERSION_ID:-}")

    case "$id" in
        fedora)
            case "$version" in
                43 | 44) ;;
                *) unsupported "Fedora ${version:-không rõ phiên bản}" ;;
            esac
            manager=dnf
            pattern="fcitx5-ngosen-[0-9][^ /]*\\.fc${version}\\.x86_64\\.rpm"
            return
            ;;
        opensuse-tumbleweed)
            manager=zypper
            pattern='fcitx5-ngosen-[0-9][^ /]*\.opensuse-tumbleweed\.x86_64\.rpm'
            return
            ;;
        arch | cachyos | endeavouros)
            manager=pacman
            pattern='fcitx5-ngosen-[0-9][^ /]*-x86_64\.pkg\.tar\.zst'
            return
            ;;
    esac

    case " $like " in
        *" arch "*)
            manager=pacman
            pattern='fcitx5-ngosen-[0-9][^ /]*-x86_64\.pkg\.tar\.zst'
            return
            ;;
    esac

    if command -v apt-get >/dev/null 2>&1; then
        case "$codename" in
            bookworm | trixie | jammy | noble | resolute)
                manager=apt
                pattern="fcitx5-ngosen_[0-9][^ /]*_${codename}_amd64\\.deb"
                return
                ;;
        esac
    fi

    unsupported "${id:-bản phân phối này} ${codename:-$version}"
}

confirm() {
    [ "$assume_yes" -eq 1 ] && return 0
    # The script itself arrives on stdin when piped from curl, so the answer is read from the terminal.
    if ! { exec 3</dev/tty; } 2>/dev/null; then
        die "không có cửa sổ dòng lệnh để hỏi lại. Chạy lại với --yes nếu đồng ý cài."
    fi
    local answer
    printf '%s [c/K] ' "$1"
    read -r answer <&3 || answer=
    exec 3<&-
    case "$answer" in
        c | C | y | Y) return 0 ;;
        *) return 1 ;;
    esac
}

# Prints "<sha256> <file name>" for the one package matching this distribution.
find_package() {
    local sums line count
    sums=$(curl -fsSL "$RELEASE_URL/SHA256SUMS") ||
        die "không tải được danh sách gói từ $RELEASE_URL"
    line=$(printf '%s\n' "$sums" | grep -E "^[0-9a-f]{64}  ${pattern}\$" || true)
    count=$(printf '%s\n' "$line" | grep -c . || true)
    [ "$count" -eq 1 ] || die "bản phát hành mới nhất không có đúng một gói cho máy này (tìm thấy $count)"
    printf '%s %s\n' "${line%% *}" "${line##* }"
}

download_package() {
    local hash=$1 file=$2 got
    curl -fL --progress-bar -o "$workdir/$file" "$RELEASE_URL/$file" ||
        die "không tải được $file"
    got=$(sha256sum "$workdir/$file" | cut -d' ' -f1)
    [ "$got" = "$hash" ] || die "tệp $file tải về không khớp mã băm trong SHA256SUMS, không cài"
}

install_with_pacman() {
    local package=$1
    local -a old=() overwrite=() path_list=()

    # pacman answers "no" to the question about removing a conflicting package, so remove it first.
    mapfile -t old < <(pacman -Qq | grep -x -E 'fcitx5-lotus(-openrc|-runit)?(-git|-bin)?' || true)
    if [ "${#old[@]}" -gt 0 ]; then
        say "Gỡ gói cũ: ${old[*]}"
        as_root pacman -R --noconfirm "${old[@]}"
    fi

    # A build installed with "cmake --install" left the same files owned by no package. Only the
    # paths this package ships may be overwritten.
    if [ -e "/usr/bin/$SERVICE" ] && ! pacman -Qo "/usr/bin/$SERVICE" >/dev/null 2>&1; then
        say "Máy có bản cài từ mã; gói sẽ ghi đè các tệp của bản đó."
        mapfile -t path_list < <(bsdtar -tf "$package" | grep -v -E '^\.|/$')
        local path
        for path in "${path_list[@]}"; do
            overwrite+=(--overwrite "$path")
        done
    fi

    as_root pacman -U --noconfirm "${overwrite[@]}" "$package"
}

install_package() {
    local package=$1
    case "$manager" in
        dnf)
            # --allowerasing lets dnf remove fcitx5-lotus in the same transaction; a newer upstream
            # version is not obsoleted by this package and would otherwise block the install.
            if rpm -q fcitx5-lotus >/dev/null 2>&1; then
                as_root dnf install -y --allowerasing "$package"
            else
                as_root dnf install -y "$package"
            fi
            ;;
        zypper)
            if rpm -q fcitx5-lotus >/dev/null 2>&1; then
                as_root zypper --non-interactive install --allow-unsigned-rpm --force-resolution "$package"
            else
                as_root zypper --non-interactive install --allow-unsigned-rpm "$package"
            fi
            ;;
        apt)
            as_root apt-get install -y "$package"
            ;;
        pacman)
            install_with_pacman "$package"
            ;;
    esac
}

# The server is a per-user service; installing the package neither enables nor restarts it.
start_server() {
    local user=${SUDO_USER:-$(id -un)}
    if [ "$user" = root ]; then
        say "Đang chạy bằng root nên chưa bật máy chủ nền cho tài khoản nào. Bằng tài khoản thường, chạy:"
        say "  sudo systemctl enable --now $SERVICE@\$(whoami).service"
        return
    fi
    if [ ! -d /run/systemd/system ]; then
        say "Máy không chạy systemd; tự bật dịch vụ $SERVICE theo hệ thống khởi động của máy."
        return
    fi
    as_root systemctl daemon-reload
    # A server left running from an older version keeps serving until it is restarted. The package
    # is already installed at this point, so a failure here is reported, not fatal.
    if as_root systemctl enable "$SERVICE@$user.service" &&
        as_root systemctl restart "$SERVICE@$user.service"; then
        say "Máy chủ nền cho $user: $(systemctl is-active "$SERVICE@$user.service" || true)"
    else
        say "Chưa bật được máy chủ nền cho $user. Xem lý do: journalctl -u $SERVICE@$user.service -n 20"
    fi
}

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -y | --yes) assume_yes=1 ;;
            -h | --help)
                usage
                exit 0
                ;;
            *) die "không hiểu tuỳ chọn $1" ;;
        esac
        shift
    done

    command -v curl >/dev/null 2>&1 || die "cần có curl"
    command -v sha256sum >/dev/null 2>&1 || die "cần có sha256sum"
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || die "cần có sudo, hoặc chạy bằng root"
    fi

    detect_distribution

    local found hash file
    found=$(find_package)
    hash=${found%% *}
    file=${found##* }
    case "$file" in
        *[!A-Za-z0-9._+~-]* | '') die "tên tệp lạ trong SHA256SUMS: $file" ;;
    esac

    say "Ngó Sen sẽ được cài bằng $manager từ gói:"
    say "  $RELEASE_URL/$file"
    say "Gói fcitx5-lotus (nếu có) sẽ bị thay; cấu hình trong ~/.config/fcitx5 được giữ."
    confirm "Tiếp tục?" || {
        say "Đã dừng, chưa cài gì."
        exit 0
    }

    workdir=$(mktemp -d)
    trap 'rm -rf "$workdir"' EXIT
    # apt reads the package as an unprivileged user.
    chmod 755 "$workdir"

    download_package "$hash" "$file"
    install_package "$workdir/$file"
    start_server

    say ""
    say "Xong. Việc còn lại:"
    say "  1. Khởi động lại fcitx5:  fcitx5 -rd"
    say "  2. Mở 'Fcitx5 Configuration', thêm bộ gõ Ngó Sen."
    say "  3. KDE Wayland: System Settings → Virtual Keyboard → chọn 'Fcitx 5'."
}

main "$@"
