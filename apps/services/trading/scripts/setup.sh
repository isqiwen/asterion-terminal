#!/bin/bash
# Installs, upgrades and configures the trading service on a Linux host.
#
#   sudo bash asterion-trading-setup.sh       install, or upgrade an installation
#   sudo /opt/asterion/trading/bin/setup.sh   add, change or disable an account
#
# The release, asterion-trading-setup.sh, is this script with the
# installation appended as a tar.gz after a line "__ARCHIVE__" (built by the
# CMake target asterion_trading_setup); it unpacks that into a temporary
# directory. The installed copy, bin/setup.sh, carries nothing and only
# changes the accounts.
#
# The installation is /opt/asterion/trading: bin/ lib/ config/ logs/ data/.
# Each CTP account runs as its own instance, the systemd service
# asterion-trading@<account>, under the account that ran the install with
# sudo; its trading record is data/<account>/. The first install creates this
# host's TLS certificate (config/tls/) and shows its fingerprint; an account
# names the Terminals that may connect by theirs. CTP credentials are entered
# locally and encrypted by systemd-creds, separately for each account.
#
# An upgrade replaces only bin/ and lib/. Instances running during trading
# hours (Beijing weekdays 08:45-15:30 and 20:45-02:45, holidays included, the
# recorder's rule) are not restarted: a timer restarts them when the window
# ends. A restart ends the account's authorization.
set -euo pipefail

ROOT=/opt/asterion/trading
UNITS=/etc/systemd/system
CREDENTIALS=/etc/credstore.encrypted
TEMPLATE=asterion-trading@.service
RESTART=asterion-trading-restart
ACCOUNT=
PROGRAMS=(bin/asterion_trading bin/setup.sh lib/ctp-trader.so)
SELF=$(readlink -f "${BASH_SOURCE[0]}")

# Defaults for new configurations only; existing instances keep their own values.
declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7460'
    [terminals]=''
    [environment]='simulation'
    [broker.broker_id]='9999'
    [broker.user_id]=''
    [broker.app_id]='simnow_client_test'
    [broker.front]='tcp://182.254.243.31:40001'
)

logging_settings() {
    cat <<'JSON'
{
  "console": {
    "enabled": false,
    "level": "info"
  },
  "file": {
    "enabled": true,
    "level": "info",
    "path": "../logs/{program}.log",
    "retention_days": 30
  }
}
JSON
}

NAME='^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$'

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

# Each instance owns its pending restart. Stopping a timer also removes a
# completed transient service so the same instance can be scheduled again.
cancel_restart() {
    systemctl stop "$RESTART-$1.timer" "$RESTART-$1.service" 2>/dev/null || true
}

restart() {
    local name unit at
    for name in "$@"; do
        unit="asterion-trading@$name.service"
        at=now
        if systemctl is-active --quiet "$unit"; then
            # shellcheck disable=SC2046
            at=$(restart_time_at $(TZ=Asia/Shanghai date +'%u %H%M'))
        fi
        cancel_restart "$name"
        if [[ $at == now ]]; then
            systemctl restart "$unit"
            echo "Started: $unit. Logs in $ROOT/logs"
        else
            case $at in
                "+1 "*) at="$(TZ=Asia/Shanghai date -d tomorrow +%F) ${at#+1 }" ;;
                *) at="$(TZ=Asia/Shanghai date +%F) $at" ;;
            esac
            systemd-run --quiet --collect --unit="$RESTART-$name" --on-calendar="$at:00 Asia/Shanghai" systemctl restart "$unit"
            echo "Account $name: saved; restart scheduled at $at Beijing time. Authorization must then be given again. To cancel: systemctl stop $RESTART-$name.timer"
        fi
    done
}

# The accounts configured on this host.
accounts() {
    local file
    for file in "$ROOT"/config/accounts/*.json; do
        [[ -e $file ]] && basename "$file" .json
    done
}

# Account values come from its own file, or the script defaults for new accounts.
value() {
    local file="$ROOT/config/accounts/$1.json"
    if [[ ! -f $file ]]; then
        if [[ $2 == port ]]; then
            free_port
            return
        fi
        printf '%s' "${DEFAULTS[$2]}"
        return
    fi
    python3 -I -c '
import json, sys
with open(sys.argv[1]) as stream:
    value = json.load(stream)
for key in sys.argv[2].split("."):
    value = value[key]
print(" ".join(value) if isinstance(value, list) else value)' "$file" "$2"
}

# The lowest unused account port, starting at the configured default.
free_port() {
    local port used other
    port=${DEFAULTS[port]}
    used=" $(accounts | while read -r other; do value "$other" port; echo; done | tr '\n' ' ') "
    while [[ $used == *" $port "* ]]; do
        port=$((port + 1))
    done
    echo "$port"
}

fingerprint() {
    openssl x509 -noout -fingerprint -sha256 -in "$ROOT/config/tls/server.crt" | sed 's/^.*=//'
}

# ask PROMPT DEFAULT PATTERN: reads a value, offering the default.
ask() {
    local value
    while true; do
        read -r -p "$1 [$2]: " value || fail "Input ended"
        value=${value:-$2}
        if [[ $value =~ $3 ]]; then
            printf '%s' "$value"
            return
        fi
        echo "  Invalid value, please try again" >&2
    done
}

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

configure_account() {
    local name bind port terminals other environment broker user app front password='' auth_code='' answer=''
    local file config identity_changed=1 credentials
    name=$(ask "Account name (as the Terminal names its instance)" "" "$NAME")
    file=$ROOT/config/accounts/$name.json
    credentials=$CREDENTIALS/asterion-trading-$name
    bind=$(ask "Listen address" "$(value "$name" bind)" '^[0-9A-Fa-f.:]+$')
    port=$(ask "Listen port" "$(value "$name" port)" '^[0-9]{1,5}$')
    port=$((10#$port))
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    for other in $(accounts); do
        if [[ $other != "$name" && $(value "$other" port) == "$port" ]]; then
            fail "Port $port is already configured for account $other"
        fi
    done
    echo "The Terminal shows its certificate fingerprint under Settings > CTP accounts."
    terminals=$(ask "Terminal fingerprints (space separated)" "$(value "$name" terminals)" \
        '^([0-9A-Fa-f:]{64,95})( [0-9A-Fa-f:]{64,95})*$')
    environment=$(ask "Counter environment (real/simulation)" "$(value "$name" environment)" '^(real|simulation)$')
    broker=$(ask "CTP broker ID" "$(value "$name" broker.broker_id)" '^[A-Za-z0-9_]{1,10}$')
    user=$(ask "CTP investor ID" "$(value "$name" broker.user_id)" '^[A-Za-z0-9_]{1,15}$')
    app=$(ask "CTP AppID" "$(value "$name" broker.app_id)" '^[A-Za-z0-9_.-]{1,32}$')
    front=$(ask "CTP trading front" "$(value "$name" broker.front)" '^tcp://[^[:space:]]+:[0-9]+$')
    if [[ -f $file ]] && [[ $environment == "$(value "$name" environment)" &&
        $broker == "$(value "$name" broker.broker_id)" && $user == "$(value "$name" broker.user_id)" &&
        $app == "$(value "$name" broker.app_id)" && $front == "$(value "$name" broker.front)" ]]; then
        identity_changed=
    fi
    # Credentials of the same counter account may be kept; another account needs its own.
    if [[ -z $identity_changed && -f $credentials/password && -f $credentials/auth_code ]]; then
        read -r -p "Keep the saved CTP password and authentication code? [Y/n]: " answer
        [[ $answer == [nN]* ]] || answer=keep
    fi
    if [[ $answer != keep ]]; then
        password=$(secret "CTP trading password (not shown)")
        auth_code=$(secret "CTP authentication code (not shown)")
    fi

    # Everything is asked; now the files, each staged and moved into place.
    stage config "$file"
    python3 -I -c '
import json, sys, pathlib
path, final, bind, port, terminals, environment, broker, user, app, front = sys.argv[1:]
identity = {"environment": environment, "broker": {"broker_id": broker, "user_id": user, "app_id": app, "front": front}}
original = pathlib.Path(final)
if original.exists() and (original.parents[2] / "data" / original.stem / "journal.sqlite").exists():
    previous = json.loads(original.read_text())
    if any(previous[key] != value for key, value in identity.items()):
        sys.exit("setup: account identity is fixed by its existing journal; restore its original configuration")
prints = []
for item in terminals.split():
    digits = item.replace(":", "").lower()
    if len(digits) != 64 or any(c not in "0123456789abcdef" for c in digits):
        sys.exit("setup: a fingerprint is 64 hex digits")
    prints.append(digits)
with open(path, "w") as output:
    json.dump({"bind": bind, "port": int(port), "terminals": prints, **identity}, output, indent=2)
    output.write("\n")' "$config" "$file" "$bind" "$port" "$terminals" "$environment" "$broker" "$user" "$app" "$front"
    chown "$ACCOUNT:" "$config"
    chmod 0600 "$config"
    if [[ -n $password ]]; then
        local encrypted_password encrypted_auth_code
        install -d -m 0700 "$credentials"
        stage encrypted_password "$credentials/password"
        stage encrypted_auth_code "$credentials/auth_code"
        printf '%s' "$password" | systemd-creds encrypt --name=password - "$encrypted_password" >/dev/null
        printf '%s' "$auth_code" | systemd-creds encrypt --name=auth_code - "$encrypted_auth_code" >/dev/null
        password='' auth_code=''
        mv -f "$encrypted_password" "$credentials/password"
        mv -f "$encrypted_auth_code" "$credentials/auth_code"
    fi
    mv -f "$config" "$file"
    systemctl enable "asterion-trading@$name.service" >/dev/null 2>&1
    echo "Saved $file"
    echo "In the Terminal, give account $name the address of this host, port $port and this host's fingerprint:"
    echo "  $(fingerprint)"
    echo "The instance listens on TCP $port with TLS: allow the port only from your Terminals, e.g. ufw allow from <address> to any port $port proto tcp"
    restart "$name"
}

disable_account() {
    local name answer
    name=$(ask "Account to stop and disable" "" "$NAME")
    [[ -f $ROOT/config/accounts/$name.json ]] || fail "No account $name on this host"
    read -r -p "Stop $name now? Working orders stay at the broker and its authorization ends. [y/N]: " answer
    [[ $answer == [yY]* ]] || return 0
    cancel_restart "$name"
    systemctl disable --now "asterion-trading@$name.service" >/dev/null 2>&1
    echo "Stopped and disabled $name. Its configuration and its trading record in $ROOT/data/$name are kept."
}

# systemd remains the owner of runtime, boot and pending-restart state.
list_accounts() {
    local name unit state enabled pending
    printf '%-20s %-24s %-14s %-14s %s\n' "ACCOUNT" "LISTEN" "STATE" "AUTOSTART" "RESTART"
    for name in $(accounts); do
        unit="asterion-trading@$name.service"
        state=$(systemctl show "$unit" --property=ActiveState --value)
        enabled=$(systemctl is-enabled "$unit" 2>/dev/null) || true
        pending=none
        if systemctl is-active --quiet "$RESTART-$name.timer"; then
            pending=$(systemctl show "$RESTART-$name.timer" --property=NextElapseUSecRealtime --value)
        fi
        printf '%-20s %-24s %-14s %-14s %s\n' "$name" "$(value "$name" bind ""):$(value "$name" port "")" "${state:-unknown}" "${enabled:-unknown}" "${pending:-scheduled}"
    done
}

menu() {
    local choice
    list_accounts
    echo "  1) Add or change an account"
    echo "  2) Stop and disable an account"
    echo "  3) Show this host's certificate fingerprint"
    echo "  4) Refresh account status"
    read -r -p "Choose [1]: " choice
    case ${choice:-1} in
        1) configure_account ;;
        2) disable_account ;;
        3) fingerprint ;;
        4) list_accounts ;;
        *) fail "Unknown choice" ;;
    esac
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion trading service, account %i
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
# It finds config/, lib/, logs/ and data/ beside its bin/.
ExecStart=$ROOT/bin/asterion_trading --account %i
ReadWritePaths=$ROOT/logs $ROOT/data
LoadCredentialEncrypted=password:$CREDENTIALS/asterion-trading-%i/password
LoadCredentialEncrypted=auth_code:$CREDENTIALS/asterion-trading-%i/auth_code
Restart=always
RestartSec=5
TimeoutStopSec=30
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

# install: the directories, the default logging settings, this host's
# certificate and the unit template.
install_once() {
    mkdir -p "$ROOT"/{bin,lib,config/accounts,config/tls,logs,data}
    chmod 0700 "$ROOT/config" "$ROOT/config/accounts" "$ROOT/config/tls" "$ROOT/logs" "$ROOT/data"
    chown "$ACCOUNT:" "$ROOT/config" "$ROOT/config/accounts" "$ROOT/config/tls" "$ROOT/logs" "$ROOT/data"
    if [[ ! -f $ROOT/config/logging.json ]]; then
        local logging
        stage logging "$ROOT/config/logging.json"
        logging_settings >"$logging"
        chown "$ACCOUNT:" "$logging"
        mv -f "$logging" "$ROOT/config/logging.json"
    fi
    if [[ ! -f $ROOT/config/tls/server.crt ]]; then
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 36500 \
            -subj "/CN=asterion-trading" -keyout "$ROOT/config/tls/server.key" \
            -out "$ROOT/config/tls/server.crt" 2>/dev/null
        chown "$ACCOUNT:" "$ROOT/config/tls/server.key" "$ROOT/config/tls/server.crt"
        chmod 0600 "$ROOT/config/tls/server.key"
    fi
    write_unit
}

# The unit template of this release, on install and on every upgrade.
write_unit() {
    local unit
    stage unit "$UNITS/$TEMPLATE"
    unit_text >"$unit"
    chmod 0644 "$unit"
    mv -f "$unit" "$UNITS/$TEMPLATE"
    systemctl daemon-reload
}

# Replace programs atomically. Running instances keep their files until they restart.
place() {
    local name
    "$1/bin/asterion_trading" --version >/dev/null 2>&1 || fail "The trading service in this release does not run on this host"
    local next
    for name in "${PROGRAMS[@]}"; do
        stage next "$ROOT/$name"
        install -m 0755 "$1/$name" "$next"
        mv -f "$next" "$ROOT/$name"
    done
}

main() {
    [[ $EUID -eq 0 ]] || fail "Run it with sudo"
    local source line
    line=$(LC_ALL=C awk '/^__ARCHIVE__$/ { print NR + 1; exit }' "$SELF")
    if [[ -n $line ]]; then
        unpacked=$(mktemp -d)
        CLEANUP+=("$unpacked")
        tail -n +"$line" "$SELF" | tar xz --no-same-owner -C "$unpacked"
        source=$unpacked/asterion-trading
    elif [[ $(dirname "$SELF") == "$ROOT/bin" ]]; then
        source=$ROOT
    else
        fail "This copy carries no programs: install or upgrade with the released asterion-trading-setup.sh"
    fi
    if [[ -d $ROOT/data ]]; then
        ACCOUNT=$(stat -c %U "$ROOT/data")
    else
        ACCOUNT=${SUDO_USER:-}
        [[ -n $ACCOUNT && $ACCOUNT != root ]] || fail "Run it with sudo from your own account: the trading service will run as that account"
    fi
    if [[ $source == "$ROOT" ]]; then
        menu
    elif [[ -x $ROOT/bin/asterion_trading ]]; then
        place "$source"
        write_unit
        echo "Upgraded the programs in $ROOT; the configuration files are unchanged."
        local running=() name
        for name in $(accounts); do
            systemctl is-active --quiet "asterion-trading@$name.service" && running+=("$name")
        done
        restart "${running[@]}"
    else
        install_once "$source"
        place "$source"
        echo "Installed in $ROOT; the trading service runs as $ACCOUNT."
        echo "This host's certificate fingerprint: $(fingerprint)"
        configure_account
    fi
}

# Bash must stop here, run or sourced: the release's archive follows.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
    exit
fi
return
