#!/bin/bash
# Installs, upgrades and configures Trading on a service host.
#
#   sudo bash Asterion-Trading-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/trading/bin/setup.sh   add, change or disable an account
#   sudo /opt/asterion/trading/bin/setup.sh fingerprint   show this host's certificate fingerprint
#
# The release, Asterion-Trading-VERSION-linux-x86_64.sh, is this script followed
# by apps/services/common/scripts/setup.sh, with the installation appended as
# a tar.gz after a line "__ARCHIVE__" (built by the CMake target
# asterion_trading_setup). The installed copy, bin/setup.sh, carries nothing
# and only changes the accounts.
#
# The installation is /opt/asterion/trading: bin/ lib/ runtime/ config/ logs/ data/.
# Each CTP account runs as its own instance, the systemd service
# asterion-trading@<account>, under the account that ran the install with
# sudo; its trading record is data/<account>/. The first install creates this
# host's TLS certificate (config/tls/) and shows its fingerprint; an account
# names the Terminals that may connect by theirs. CTP credentials are entered
# locally and encrypted by systemd-creds, separately for each account.
#
# An upgrade requires every account to be stopped, then replaces bin/, lib/
# and runtime/. It never stops or starts accounts. Configuration edits alone
# may schedule a restart after trading hours. A restart ends authorization.
set -euo pipefail

NAME=Trading
PACKAGE=asterion-trading
ROOT=/opt/asterion/trading
UNIT=asterion-trading@.service
RESTART=asterion-trading-restart

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
    [market_front]=''
)

ACCOUNT_NAME='^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$'

python() {
    "$ROOT/runtime/python/bin/python3" "$@"
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
    if [[ ! -f $file && $2 == port ]]; then
        free_port
        return
    fi
    setting "$file" "$2" "${DEFAULTS[$2]}"
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

configure_account() {
    local name bind port terminals other environment broker user app front market_front password='' auth_code=''
    local file config credentials encrypted_password encrypted_auth_code
    name=$(ask "Account name (as the Terminal names its instance)" "" "$ACCOUNT_NAME")
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
    terminals=$(ask "Terminal fingerprints (space separated)" "$(value "$name" terminals)" '^[0-9A-Fa-f:[:space:]]+$')
    terminals=$(pins "$terminals")
    environment=$(ask "Counter environment (real/simulation)" "$(value "$name" environment)" '^(real|simulation)$')
    broker=$(ask "CTP broker ID" "$(value "$name" broker.broker_id)" '^[A-Za-z0-9_]{1,10}$')
    user=$(ask "CTP investor ID" "$(value "$name" broker.user_id)" '^[A-Za-z0-9_]{1,15}$')
    app=$(ask "CTP AppID" "$(value "$name" broker.app_id)" '^[A-Za-z0-9_.-]{1,32}$')
    front=$(ask "CTP trading front" "$(value "$name" broker.front)" '^tcp://[^[:space:]]+:[0-9]+$')
    market_front=$(ask "CTP market front" "$(value "$name" market_front)" '^tcp://[A-Za-z0-9.-]+:[0-9]+$')
    # Credentials of the same counter account may be kept; another account needs its own.
    if ! [[ -f $file && $environment == "$(value "$name" environment)"
        && $broker == "$(value "$name" broker.broker_id)" && $user == "$(value "$name" broker.user_id)"
        && $app == "$(value "$name" broker.app_id)" && $front == "$(value "$name" broker.front)"
        && -f $credentials/password && -f $credentials/auth_code ]] ||
        ! confirm "Keep the saved CTP password and authentication code?" y; then
        password=$(secret "CTP trading password (not shown)")
        auth_code=$(secret "CTP authentication code (not shown)")
    fi

    # Everything is asked; now the files, each staged and moved into place.
    stage config "$file"
    python -I -c '
import json, sys, pathlib
path, final, bind, port, terminals, environment, broker, user, app, front, market_front = sys.argv[1:]
identity = {"environment": environment, "broker": {"broker_id": broker, "user_id": user, "app_id": app, "front": front}}
original = pathlib.Path(final)
if original.exists() and (original.parents[2] / "data" / original.stem / "journal.sqlite").exists():
    previous = json.loads(original.read_text())
    if any(previous[key] != value for key, value in identity.items()):
        sys.exit("setup: account identity is fixed by its existing journal; restore its original configuration")
with open(path, "w") as output:
    json.dump({"bind": bind, "port": int(port), "terminals": terminals.split(), "market_front": market_front, **identity}, output, indent=2)
    output.write("\n")' "$config" "$file" "$bind" "$port" "$terminals" "$environment" "$broker" "$user" "$app" "$front" "$market_front"
    private "$config"
    if [[ -n $password ]]; then
        install -d -m 0700 "$credentials"
        seal encrypted_password "$credentials/password" password "$password"
        seal encrypted_auth_code "$credentials/auth_code" auth_code "$auth_code"
        password='' auth_code=''
        mv -f "$encrypted_password" "$credentials/password"
        mv -f "$encrypted_auth_code" "$credentials/auth_code"
    fi
    mv -f "$config" "$file"
    systemctl enable "asterion-trading@$name.service" >/dev/null 2>&1
    echo "Saved $file"
    echo "In Terminal, give account $name the address of this host and port $port."
    fingerprint
    firewall "$port"
    restart_after_trading "asterion-trading@$name.service" "$RESTART-$name"
    echo "A restart ends authorization: authorize $name again in Terminal."
}

disable_account() {
    local name answer
    name=$(ask "Account to stop and disable" "" "$ACCOUNT_NAME")
    [[ -f $ROOT/config/accounts/$name.json ]] || fail "No account $name on this host"
    read -r -p "Stop $name now? Working orders stay at the broker and its authorization ends. [y/N]: " answer
    [[ $answer == [yY]* ]] || return 0
    cancel_restart "$RESTART-$name"
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
    install_base "$1"
    mkdir -p "$ROOT"/{lib,config/accounts,data}
    chmod 0700 "$ROOT/config/accounts" "$ROOT/data"
    chown "$ACCOUNT:" "$ROOT/config/accounts" "$ROOT/data"
    write_unit
}

# The caller holds the exclusive installation lock. A failed or interrupted
# replacement remains closed to account startup until setup succeeds again.
place() {
    local name next
    next=$(mktemp -d "$ROOT/.program.XXXXXX")
    CLEANUP+=("$next")
    for name in bin lib runtime; do
        cp -a "$1/$name" "$next/$name"
    done
    chmod -R a+rX,go-w "$next"
    chmod 0755 "$next/bin/asterion_trading" "$next/bin/setup.sh"
    "$next/bin/asterion_trading" --version >/dev/null 2>&1 || fail "Trading in this release does not run on this host"
    "$next/runtime/python/bin/python3" -I -B -c \
        'import sys, numpy, google.protobuf; assert sys.version_info[:3] == (3, 12, 12); assert numpy.__version__ == "2.3.5"; assert google.protobuf.__version__ == "5.29.3"; sys.path.insert(0, sys.argv[1]); import asterion_strategy, asterion.strategy_pb2' \
        "$next/lib/strategy" || fail "The strategy runtime in this release does not run on this host"
    touch "$ROOT/.program-incomplete"
    for name in bin lib runtime; do
        rm -rf "$ROOT/$name"
        mv "$next/$name" "$ROOT/$name"
    done
    write_unit
    rm "$ROOT/.program-incomplete"
}

setup() {
    if [[ $1 == "$ROOT" ]]; then
        menu
    else
        mkdir -p "$ROOT"
        # Stable inode shared by all accounts and never replaced with programs.
        touch "$ROOT/.program.lock"
        chmod 0644 "$ROOT/.program.lock"
        local program_lock name
        exec {program_lock}<"$ROOT/.program.lock"
        flock --exclusive --nonblock "$program_lock" || fail "Stop all Trading accounts on this host before upgrading; no programs were changed"
        if [[ -d $ROOT/data ]]; then
            for name in $(accounts); do cancel_restart "$RESTART-$name"; done
            place "$1"
            echo "Upgraded programs and offline strategy runtime in $ROOT; configuration and data unchanged. Start accounts explicitly and authorize again."
        else
            install_once "$1"
            place "$1"
            # The configured account may start only after installation completes.
            exec {program_lock}<&-
            echo "Installed in $ROOT; Trading runs as $ACCOUNT."
            fingerprint
            configure_account
        fi
    fi
}
