#!/bin/bash
# Installs, upgrades and configures Market on a service host.
#
#   sudo bash Asterion-Market-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/market/bin/setup.sh   change the settings
#   sudo /opt/asterion/market/bin/setup.sh fingerprint   show this host's certificate fingerprint
#
# The release, Asterion-Market-VERSION-linux-x86_64.sh, is this script with the
# installation appended as a tar.gz after a line "__ARCHIVE__" (built by the
# CMake target asterion_market_setup); it unpacks that into a temporary
# directory. The installed copy, bin/setup.sh, carries nothing and only
# changes the settings.
#
# The installation is /opt/asterion/market: bin/ lib/ config/ logs/, and
# data/ unless the settings name another data directory.
# A first install asks for the settings; an upgrade replaces only bin/ and
# lib/ and leaves config/ (market.json, logging.json) as it is
# (it offers to change the settings).
# Settings questions offer the current values as defaults; the password and
# the authentication code have none and are encrypted with systemd-creds,
# never written to a file in the clear. Market runs as the system
# service asterion-market under the account that ran the install with sudo.
#
# While Market serves during trading hours (Beijing weekdays
# 08:45-15:30 and 20:45-02:45, holidays included), it is not restarted: the
# change is saved now and a timer restarts it when the trading window ends.
set -euo pipefail

ROOT=/opt/asterion/market
UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
UNIT=asterion-market.service
RESTART=asterion-market-restart
# The account Market runs as: the sudo user at install, the owner of
# config/ afterwards.
ACCOUNT=
PROGRAMS=(bin/asterion_market bin/asterion_crash_reporter bin/setup.sh lib/ctp-md.so lib/ctp-trader.so)
# This script: the release with its archive, or the installed bin/setup.sh.
SELF=$(readlink -f "${BASH_SOURCE[0]}")

# Defaults for new configurations only; existing instances keep their own values.
declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7451'
    [clients]=''
    [counter.market_front]='tcp://182.254.243.31:40011'
    [counter.trade_front]='tcp://182.254.243.31:40001'
    [counter.broker]='9999'
    [counter.user]=''
    [counter.app_id]='simnow_client_test'
)

fail() {
    echo "setup: $*" >&2
    exit 1
}

# Files this run writes before moving them into place, and the unpacked
# release: removed however the script ends, so nothing half written stays.
CLEANUP=()
cleanup() {
    ((${#CLEANUP[@]})) && rm -rf "${CLEANUP[@]}"
    return 0
}
trap cleanup EXIT

# stage VARIABLE PATH: sets VARIABLE to a fresh name beside PATH for its next
# version, removed on exit unless moved into place. It runs in this shell, not
# in a command substitution, so the name reaches CLEANUP.
stage() {
    local file
    file=$(mktemp "$(dirname "$2")/.$(basename "$2").XXXXXX")
    CLEANUP+=("$file")
    printf -v "$1" '%s' "$file"
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

# When Market may restart: "now" (it is not running, or it is outside
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
        echo "Market has (re)started. Status: systemctl status $UNIT; logs in $ROOT/logs"
    else
        systemd-run --quiet --unit="$RESTART" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$UNIT"
        echo "Market is serving during trading hours. The change is saved and takes effect when it restarts automatically at $at Beijing time. To cancel: systemctl stop $RESTART.timer"
    fi
}

# The default of a key; the data directory is beside the programs.
default() {
    if [[ $1 == data ]]; then printf '%s' "$ROOT/data"; else printf '%s' "${DEFAULTS[$1]}"; fi
}

# The current value of a key ("port", "counter.broker") in the settings, or
# its default where the settings have none.
current() {
    if [[ ! -f $ROOT/config/market.json ]]; then
        default "$1"
        return
    fi
    python3 -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    if key not in value:
        print(sys.argv[3])
        raise SystemExit
    value = value[key]
if sys.argv[2] == "clients":
    value = " ".join(item["fingerprint"] for item in value)
print(value)' "$ROOT/config/market.json" "$1" "$(default "$1")"
}

# ask KEY PROMPT PATTERN: reads a value, offering the current one.
ask() {
    local prompt=$2 pattern=$3 default value
    default=$(current "$1")
    while true; do
        read -r -p "$prompt [${default}]: " value || fail "Input ended"
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
        read -r -s -p "$1: " value || fail "Credential input ended"
        echo >&2
        [[ -n $value ]] && break
        echo "  Required" >&2
    done
    printf '%s' "$value"
}

configure() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' bind port data previous tick_days
    local market_front trade_front broker user app_id
    local password='' auth_code='' settings encrypted_password='' encrypted_auth_code=''
    local clients answer=''
    bind=$(ask bind "Listen address" '^[0-9A-Fa-f.:]+$')
    port=$(ask port "Listen port" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    previous=$(current data)
    data=$(ask data "Data directory" '^/[^[:space:]]*$')
    echo "Terminal shows its certificate fingerprint under Settings > About."
    clients=$(ask clients "Terminal fingerprints (space separated)" '^[0-9A-Fa-f:[:space:]]+$')
    echo "Use a CTP account of its own for quotes, never a live trading account."
    market_front=$(ask counter.market_front "CTP market front" "$front")
    trade_front=$(ask counter.trade_front "CTP trading front" "$front")
    broker=$(ask counter.broker "CTP broker ID" '^[A-Za-z0-9]+$')
    user=$(ask counter.user "CTP investor ID" '^[A-Za-z0-9]+$')
    app_id=$(ask counter.app_id "CTP AppID (- for none)" '^[A-Za-z0-9_-]*$')
    [[ $app_id == - ]] && app_id=
    # Credentials of the same counter account may be kept; another account needs its own.
    if [[ -f $ROOT/config/market.json && $broker == "$(current counter.broker)" && $user == "$(current counter.user)"
        && $app_id == "$(current counter.app_id)" && -f $CREDENTIALS/asterion.ctp.password
        && ( -z $app_id || -f $CREDENTIALS/asterion.ctp.auth_code ) ]]; then
        read -r -p "Keep the saved CTP password and authentication code? [Y/n]: " answer
        [[ $answer == [nN]* ]] || answer=keep
    fi
    if [[ $answer != keep ]]; then
        password=$(secret "CTP password (not shown)")
        [[ -n $app_id ]] && auth_code=$(secret "CTP authentication code (not shown)")
    fi

    # Replaced whole, every file staged first; only the market's account may read it.
    stage settings "$ROOT/config/market.json"
    python3 -I -c '
import json, sys
bind, port, data, market, trade, broker, user, app_id, clients = sys.argv[2:]
import re
peers = []
for pin in clients.split():
    pin = pin.replace(":", "").lower()
    if not re.fullmatch("[a-f0-9]{64}", pin) or any(p["fingerprint"] == pin for p in peers):
        raise SystemExit("Invalid or duplicate client fingerprint")
    peers.append({"fingerprint": pin})
if not 1 <= len(peers) <= 64:
    raise SystemExit("Register 1-64 clients")
settings = {"bind": bind, "port": int(port), "data": data, "clients": peers, "counter": {
    "market_front": market, "trade_front": trade, "broker": broker, "user": user, "app_id": app_id}}
with open(sys.argv[1], "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$settings" "$bind" "$port" "$data" "$market_front" "$trade_front" \
        "$broker" "$user" "$app_id" "$clients"
    chown "$ACCOUNT:" "$settings"
    chmod 0600 "$settings"
    if [[ -n $password ]]; then
        stage encrypted_password "$CREDENTIALS/asterion.ctp.password"
        printf '%s' "$password" |
            systemd-creds encrypt --name=asterion.ctp.password - "$encrypted_password" >/dev/null
    fi
    if [[ -n $auth_code ]]; then
        stage encrypted_auth_code "$CREDENTIALS/asterion.ctp.auth_code"
        printf '%s' "$auth_code" |
            systemd-creds encrypt --name=asterion.ctp.auth_code - "$encrypted_auth_code" >/dev/null
    fi
    password='' auth_code=''
    [[ -z $encrypted_password ]] || mv -f "$encrypted_password" "$CREDENTIALS/asterion.ctp.password"
    [[ -z $encrypted_auth_code ]] || mv -f "$encrypted_auth_code" "$CREDENTIALS/asterion.ctp.auth_code"
    mkdir -p "$data"
    chmod 0700 "$data"
    chown "$ACCOUNT:" "$data"
    mv -f "$settings" "$ROOT/config/market.json"
    echo "Saved $ROOT/config/market.json"
    # The service may write only its logs and its data directory.
    write_unit
    if [[ $data != "$previous" && -d $previous && -n $(ls -A "$previous") ]]; then
        echo "The existing data stays in $previous; it is not moved. To keep using it, stop Market and move its contents to $data."
    fi
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion live quotes
Wants=network-online.target
After=network-online.target
# Never give up restarting: a service that stays down serves nothing.
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
# It finds config/, lib/ and logs/ beside its bin/; config/market.json names its data.
ExecStart=$ROOT/bin/asterion_market
ReadWritePaths=$ROOT/logs $(current data)
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
prepare_identity() {
    local directory=$ROOT/config/tls
    if [[ -f $directory/server.crt && -f $directory/server.key ]]; then return; fi
    [[ ! -e $directory/server.crt && ! -e $directory/server.key ]] || fail "Incomplete TLS identity in $directory; preserve and inspect it"
    local staging
    staging=$(mktemp -d "$ROOT/config/.tls-XXXXXX")
    CLEANUP+=("$staging")
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 36500 \
        -subj "/CN=asterion-market" -keyout "$staging/server.key" -out "$staging/server.crt" 2>/dev/null
    chown -R "$ACCOUNT:" "$staging"
    chmod 0700 "$staging"
    chmod 0600 "$staging/server.key"
    rmdir "$directory"
    mv "$staging" "$directory"
}

install_once() {
    mkdir -p "$ROOT"/{bin,lib,config/tls,logs} "$CREDENTIALS"
    chmod 0700 "$ROOT/config" "$ROOT/logs" "$CREDENTIALS"
    chown "$ACCOUNT:" "$ROOT/config" "$ROOT/logs"
    if [[ ! -f $ROOT/config/logging.json ]]; then
        local logging
        stage logging "$ROOT/config/logging.json"
        cat "$1/config/logging.json" >"$logging"
        chown "$ACCOUNT:" "$logging"
        mv -f "$logging" "$ROOT/config/logging.json"
    fi
    prepare_identity
    write_unit
    systemctl enable "$UNIT" >/dev/null
}

# The unit of this release, on install and on every upgrade.
write_unit() {
    local unit
    stage unit "$UNITS/$UNIT"
    unit_text >"$unit"
    chmod 0644 "$unit"
    mv -f "$unit" "$UNITS/$UNIT"
    systemctl daemon-reload
}

# Replaces the programs in bin/ and lib/, one rename each: a running
# market keeps the files it started with until it restarts.
place() {
    local name next
    "$1/bin/asterion_market" --version >/dev/null 2>&1 || fail "Market in this release does not run on this host"
    for name in "${PROGRAMS[@]}"; do
        stage next "$ROOT/$name"
        install -m 0755 "$1/$name" "$next"
        mv -f "$next" "$ROOT/$name"
    done
}

fingerprint() {
    echo "This host's certificate fingerprint, to enter in Terminal:"
    echo "  $(openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt" | sed 's/^.*=//')"
}

main() {
    [[ $EUID -eq 0 ]] || fail "Run it with sudo"
    if [[ ${1:-} == fingerprint ]]; then
        fingerprint
        return
    fi
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        unpacked=$(mktemp -d)
        CLEANUP+=("$unpacked")
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$unpacked"
        source=$unpacked/asterion-market
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "This copy carries no programs: install or upgrade with the released Asterion-Market-VERSION-linux-x86_64.sh"
    fi
    if [[ -d $ROOT/config ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/config")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "Run it with sudo from your own account: Market will run as that account"
    fi
    if [[ $source == "$ROOT" ]]; then
        configure
    elif [[ -x $ROOT/bin/asterion_market ]]; then
        place "$source"
        write_unit
        echo "Upgraded the programs in $ROOT; the configuration files are unchanged."
        local answer
        read -r -p "Change the settings? [y/N]: " answer
        [[ $answer == [yY]* ]] && configure
    else
        install_once "$source"
        place "$source"
        echo "Installed in $ROOT; Market runs as $ACCOUNT. Settings:"
        configure
        port=$(current port)
        echo "Market listens on TCP $port with TLS: allow the port only from your Terminals, e.g. ufw allow from <address> to any port $port proto tcp"
    fi
    fingerprint
    echo "Registered clients take effect after restart. Credentials stay on this host."
    restart
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
