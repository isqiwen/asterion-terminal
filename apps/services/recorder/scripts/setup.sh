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
# lib/ and leaves config/ (recorder.json, recorder-logging.json) as it is
# (it offers to change the settings).
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
        echo "The recorder has (re)started. Status: systemctl status $UNIT; logs in $ROOT/logs"
    else
        systemd-run --quiet --unit="$RESTART" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$UNIT"
        echo "The recorder is recording during trading hours. The change is saved and takes effect when it restarts automatically at $at Beijing time. To cancel: systemctl stop $RESTART.timer"
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
        echo "  Invalid value, please try again" >&2
    done
}

# secret PROMPT: reads a value without echo; there is no default.
secret() {
    local value
    while true; do
        read -r -s -p "$1: " value
        echo >&2
        [[ -n $value ]] && break
        echo "  Required" >&2
    done
    printf '%s' "$value"
}

configure() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' bind port market_front trade_front broker user app_id
    local password auth_code=
    bind=$(ask bind "Listen address" '^[0-9A-Fa-f.:]+$')
    port=$(ask port "Listen port" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    market_front=$(ask counter.market_front "Market data front" "$front")
    trade_front=$(ask counter.trade_front "Trading front" "$front")
    broker=$(ask counter.broker "Broker ID" '^[A-Za-z0-9]+$')
    user=$(ask counter.user "Investor ID" '^[A-Za-z0-9]+$')
    app_id=$(ask counter.app_id "AppID (- for none)" '^[A-Za-z0-9_-]*$')
    [[ $app_id == - ]] && app_id=
    password=$(secret "Password (not shown)")
    [[ -n $app_id ]] && auth_code=$(secret "Authentication code (not shown)")

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
    echo "Saved $ROOT/config/recorder.json"
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
    # Copied once: upgrades never overwrite the configuration files.
    local name
    for name in recorder.json recorder-logging.json; do
        if [[ ! -f $ROOT/config/$name ]]; then
            install -m 0600 "$1/config/$name" "$ROOT/config/$name"
            chown "$ACCOUNT:" "$ROOT/config/$name"
        fi
    done
    unit_text >"$UNITS/$UNIT"
    systemctl daemon-reload
    systemctl enable "$UNIT" >/dev/null
}

# Replaces the programs in bin/ and lib/, one rename each: a running
# recorder keeps the files it started with until it restarts.
place() {
    local name
    "$1/bin/asterion_recorder" --version >/dev/null 2>&1 || fail "The recorder in this release does not run on this host"
    for name in "${PROGRAMS[@]}"; do
        install -m 0755 "$1/$name" "$ROOT/$name.new"
        mv -f "$ROOT/$name.new" "$ROOT/$name"
    done
}

main() {
    [[ $EUID -eq 0 ]] || fail "Run it with sudo"
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
        fail "This copy carries no programs: install or upgrade with the released asterion-recorder-setup.sh"
    fi
    if [[ -d $ROOT/data ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/data")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "Run it with sudo from your own account: the recorder will run as that account"
    fi
    if [[ $source == "$ROOT" ]]; then
        configure
    elif [[ -x $ROOT/bin/asterion_recorder ]]; then
        place "$source"
        echo "Upgraded the programs in $ROOT; the configuration files are unchanged."
        local answer
        read -r -p "Change the settings? [y/N]: " answer
        [[ $answer == [yY]* ]] && configure
    else
        install_once "$source"
        place "$source"
        echo "Installed in $ROOT; the recorder runs as $ACCOUNT. Settings:"
        configure
        port=$(current port)
        echo "The recorder listens on TCP $port without encryption: allow the port only from your own machines, e.g. ufw allow from <address> to any port $port proto tcp"
    fi
    restart
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
