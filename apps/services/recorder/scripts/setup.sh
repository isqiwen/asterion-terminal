#!/bin/bash
# Installs, upgrades and configures the recorder on a recording host.
#
#   sudo bash asterion-recorder-setup.sh       install, or upgrade an installation
#   sudo /opt/asterion-recorder/bin/setup.sh   change the settings
#
# The release, asterion-recorder-setup.sh, is this script with the
# installation appended as a tar.gz after a line "__ARCHIVE__" (built by the
# CMake target asterion_recorder_setup); it unpacks that into a temporary
# directory. The installed copy, bin/setup.sh, carries nothing and only
# changes the settings.
#
# The installation is /opt/asterion-recorder: bin/ lib/ config/ logs/ data/.
# A first install asks for the settings; an upgrade replaces only bin/ and
# lib/ and leaves config/recorder.json as it is (it offers to change it).
# Settings questions offer the current values as defaults; the password and
# the authentication code have none and are encrypted with systemd-creds,
# never written to a file in the clear. The recorder runs as the system
# service asterion-recorder under the account that ran the install with sudo.
#
# While the recorder records during trading hours (Beijing weekdays
# 08:45-15:30 and 20:45-02:45, holidays included), it is not restarted: the
# change is saved now and a timer restarts it when the trading window ends.
set -euo pipefail

ROOT=/opt/asterion-recorder
UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
UNIT=asterion-recorder.service
RESTART=asterion-recorder-restart
# The account the recorder runs as: the sudo user at install, the owner of
# data/ afterwards.
ACCOUNT=
PROGRAMS=(bin/asterion_recorder bin/setup.sh lib/ctp-md.so lib/ctp-trader.so)
# This script: the release with its archive, or the installed bin/setup.sh.
SELF=$(readlink -f "${BASH_SOURCE[0]}")

fail() {
    echo "setup: $*" >&2
    exit 1
}

# restart_time_at WEEKDAY HHMM: the end of the trading window at that Beijing
# time (1 is Monday), "+1 02:45" for tomorrow's, or "now" outside one.
restart_time_at() {
    local weekday=$1 minute=$((10#${2:0:2} * 60 + 10#${2:2:2}))
    if ((weekday <= 5 && minute >= 525 && minute < 930)); then
        echo "15:30"
    elif ((weekday <= 5 && minute >= 1245)); then
        echo "+1 02:45"
    elif ((weekday >= 2 && weekday <= 6 && minute < 165)); then
        echo "02:45"
    else
        echo now
    fi
}

# When the recorder may restart: "now" (it is not running, or it is outside
# trading hours), else the Beijing date and time the trading window ends.
restart_time() {
    systemctl is-active --quiet "$UNIT" || {
        echo now
        return
    }
    local at
    # shellcheck disable=SC2046
    at=$(restart_time_at $(TZ=Asia/Shanghai date +'%u %H%M'))
    case $at in
        now) echo now ;;
        "+1 "*) echo "$(TZ=Asia/Shanghai date -d tomorrow +%F) ${at#+1 }" ;;
        *) echo "$(TZ=Asia/Shanghai date +%F) $at" ;;
    esac
}

restart() {
    local at
    at=$(restart_time)
    # A restart an earlier run scheduled is replaced.
    systemctl stop "$RESTART.timer" 2>/dev/null || true
    if [[ $at == now ]]; then
        systemctl restart "$UNIT"
        echo "录制程序已（重新）启动。状态：systemctl status $UNIT；日志在 $ROOT/logs"
    else
        systemd-run --quiet --unit="$RESTART" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$UNIT"
        echo "录制程序正在交易时段运行，改动已保存，在北京时间 $at 自动重启后生效。取消：systemctl stop $RESTART.timer"
    fi
}

# The current value of a key ("port", "counter.broker") in the settings.
current() {
    python3 -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    value = value.get(key, "") if isinstance(value, dict) else ""
print(value)' "$ROOT/config/recorder.json" "$1"
}

# ask KEY PROMPT PATTERN: reads a value, offering the current one.
ask() {
    local prompt=$2 pattern=$3 default value
    default=$(current "$1")
    while true; do
        read -r -p "$prompt [${default}]: " value
        value=${value:-$default}
        if [[ $value =~ $pattern ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  格式不对，请重新输入" >&2
    done
}

# secret PROMPT: reads a value without echo; there is no default.
secret() {
    local value
    while true; do
        read -r -s -p "$1: " value
        echo >&2
        [[ -n $value ]] && break
        echo "  不能为空" >&2
    done
    printf '%s' "$value"
}

configure() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' bind port market_front trade_front broker user app_id
    local password auth_code=
    bind=$(ask bind "监听地址" '^[0-9A-Fa-f.:]+$')
    port=$(ask port "监听端口" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "端口必须在 1-65535 之间"
    market_front=$(ask counter.market_front "行情前置" "$front")
    trade_front=$(ask counter.trade_front "交易前置" "$front")
    broker=$(ask counter.broker "经纪公司代码" '^[A-Za-z0-9]+$')
    user=$(ask counter.user "投资者代码" '^[A-Za-z0-9]+$')
    app_id=$(ask counter.app_id "AppID（没有则输入 -）" '^[A-Za-z0-9_-]*$')
    [[ $app_id == - ]] && app_id=
    password=$(secret "密码（不显示）")
    [[ -n $app_id ]] && auth_code=$(secret "授权码（不显示）")

    # Replaced whole; only the recorder's account may read it.
    python3 -I -c '
import json, sys
bind, port, market, trade, broker, user, app_id = sys.argv[2:]
settings = {"bind": bind, "port": int(port), "counter": {
    "market_front": market, "trade_front": trade, "broker": broker, "user": user, "app_id": app_id}}
with open(sys.argv[1], "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$ROOT/config/recorder.json.new" "$bind" "$port" "$market_front" "$trade_front" \
        "$broker" "$user" "$app_id"
    chown "$ACCOUNT:" "$ROOT/config/recorder.json.new"
    chmod 0600 "$ROOT/config/recorder.json.new"
    mv "$ROOT/config/recorder.json.new" "$ROOT/config/recorder.json"

    printf '%s' "$password" |
        systemd-creds encrypt --name=asterion.ctp.password - "$CREDENTIALS/asterion.ctp.password" >/dev/null
    if [[ -n $auth_code ]]; then
        printf '%s' "$auth_code" |
            systemd-creds encrypt --name=asterion.ctp.auth_code - "$CREDENTIALS/asterion.ctp.auth_code" >/dev/null
    fi
    echo "已保存 $ROOT/config/recorder.json"
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion market recorder
Wants=network-online.target
After=network-online.target
# Never give up restarting: a recorder that stays down loses the market.
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
# It finds config/, lib/, logs/ and data/ beside its bin/.
ExecStart=$ROOT/bin/asterion_recorder
ReadWritePaths=$ROOT/logs $ROOT/data
# The counter password and authentication code, decrypted at start.
ImportCredential=asterion.ctp.*
Restart=always
RestartSec=5
TimeoutStopSec=60
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
CapabilityBoundingSet=
RestrictSUIDSGID=yes

[Install]
WantedBy=multi-user.target
EOF
}

# install: the directories, the default settings and the unit.
install_once() {
    mkdir -p "$ROOT"/{bin,lib,config,logs,data} "$CREDENTIALS"
    chmod 0700 "$ROOT/config" "$ROOT/logs" "$ROOT/data" "$CREDENTIALS"
    chown "$ACCOUNT:" "$ROOT/config" "$ROOT/logs" "$ROOT/data"
    if [[ ! -f $ROOT/config/recorder.json ]]; then
        install -m 0600 "$1/config/recorder.json" "$ROOT/config/recorder.json"
        chown "$ACCOUNT:" "$ROOT/config/recorder.json"
    fi
    unit_text >"$UNITS/$UNIT"
    systemctl daemon-reload
    systemctl enable "$UNIT" >/dev/null
}

# Replaces the programs in bin/ and lib/, one rename each: a running
# recorder keeps the files it started with until it restarts.
place() {
    local name
    "$1/bin/asterion_recorder" --version >/dev/null 2>&1 || fail "这个包里的录制程序不能在本机运行"
    for name in "${PROGRAMS[@]}"; do
        install -m 0755 "$1/$name" "$ROOT/$name.new"
        mv -f "$ROOT/$name.new" "$ROOT/$name"
    done
}

main() {
    [[ $EUID -eq 0 ]] || fail "请用 sudo 运行"
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        unpacked=$(mktemp -d)
        trap 'rm -rf "$unpacked"' EXIT
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$unpacked"
        source=$unpacked/asterion-recorder
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "这份脚本不带程序：安装或升级请运行发布的 asterion-recorder-setup.sh"
    fi
    if [[ -d $ROOT/data ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/data")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "请用你自己的账户通过 sudo 运行，录制程序将以该账户运行"
    fi
    if [[ $source == "$ROOT" ]]; then
        configure
    elif [[ -x $ROOT/bin/asterion_recorder ]]; then
        place "$source"
        echo "已升级 $ROOT 的程序，配置文件未改动。"
        local answer
        read -r -p "要修改配置吗？[y/N]: " answer
        [[ $answer == [yY]* ]] && configure
    else
        install_once "$source"
        place "$source"
        echo "已安装到 $ROOT，录制程序以 $ACCOUNT 运行。请填写配置："
        configure
        port=$(current port)
        echo "录制程序在 TCP $port 上不加密监听，防火墙只放行自己的机器，例如：ufw allow from <地址> to any port $port proto tcp"
    fi
    restart
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
