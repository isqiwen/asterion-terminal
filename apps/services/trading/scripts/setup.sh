#!/bin/bash
# Installs, upgrades and configures Trading on a service host.
#
#   sudo bash Asterion-Trading-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/trading/bin/setup.sh   add accounts; add or remove an account's Terminals, change or disable it
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
# Changes to an account are shown and saved only when confirmed, and take
# effect together when its menu ends.
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

# The account the questions and the account menu are about.
INSTANCE=

account_file() {
    echo "$ROOT/config/accounts/${1:-$INSTANCE}.json"
}

# A setting of INSTANCE from its own file, or the script default for a new
# account; a new account's port is the lowest unused one.
current() {
    if [[ ! -f $(account_file) && $1 == port ]]; then
        free_port
        return
    fi
    setting "$(account_file)" "$1" "${DEFAULTS[$1]}"
}

# port_owner PORT: the other account configured on PORT, if any.
port_owner() {
    local other
    for other in $(accounts); do
        if [[ $other != "$INSTANCE" && $(setting "$(account_file "$other")" port "") == "$1" ]]; then
            echo "$other"
            return
        fi
    done
    return 1
}

free_port() {
    local port=${DEFAULTS[port]}
    while port_owner "$port" >/dev/null; do
        port=$((port + 1))
    done
    echo "$port"
}

# configure_account: asks for INSTANCE's settings other than its Terminals
# (those too for a new account) and saves what changed.
configure_account() {
    local file credentials bind port owner terminals environment broker user app front market_front
    local identity saved_identity password='' auth_code='' config encrypted_password encrypted_auth_code
    file=$(account_file)
    credentials=$CREDENTIALS/asterion-trading-$INSTANCE
    CHANGES=()
    field bind "Listen address" bind '^[0-9A-Fa-f.:]+$'
    while true; do
        port=$(ask_number "Listen port" "$(current port)" 1 65535)
        owner=$(port_owner "$port") || break
        echo "  Port $port is already configured for account $owner" >&2
    done
    note "Listen port" "$(current port)" "$port"
    field environment "Counter environment (real/simulation)" environment '^(real|simulation)$'
    field broker "CTP broker ID" broker.broker_id '^[A-Za-z0-9_]{1,10}$'
    field user "CTP investor ID" broker.user_id '^[A-Za-z0-9_]{1,15}$'
    field app "CTP AppID" broker.app_id '^[A-Za-z0-9_.-]{1,32}$'
    field front "CTP trading front" broker.front '^tcp://[^[:space:]]+:[0-9]+$'
    field market_front "CTP market front" market_front '^tcp://[A-Za-z0-9.-]+:[0-9]+$'
    identity="$environment $broker $user $app $front"
    saved_identity="$(current environment) $(current broker.broker_id) $(current broker.user_id) $(current broker.app_id) $(current broker.front)"
    if [[ -f $file && -f $ROOT/data/$INSTANCE/journal.sqlite && $identity != "$saved_identity" ]]; then
        echo "  The trading record in $ROOT/data/$INSTANCE fixes this account's counter; nothing was saved" >&2
        return
    fi
    terminals=$(current terminals)
    [[ -f $file ]] || terminals=$(add_pins)
    # Credentials of the same counter account may be kept; another account needs its own.
    if ! [[ -f $file && $identity == "$saved_identity" && -f $credentials/password && -f $credentials/auth_code ]] ||
        ! confirm "Keep the saved CTP password and authentication code?" y; then
        password=$(secret "CTP trading password (not shown)")
        auth_code=$(secret "CTP authentication code (not shown)")
        CHANGES+=("CTP password and authentication code: replaced")
    fi
    if [[ -f $file ]] && ! review; then return; fi

    # Everything is asked; now the files, each staged and moved into place.
    stage config "$file"
    python -I -c '
import json, sys
path, bind, port, terminals, environment, broker, user, app, front, market_front = sys.argv[1:]
identity = {"environment": environment, "broker": {"broker_id": broker, "user_id": user, "app_id": app, "front": front}}
with open(path, "w") as output:
    json.dump({"bind": bind, "port": int(port), "terminals": terminals.split(), "market_front": market_front, **identity}, output, indent=2)
    output.write("\n")' "$config" "$bind" "$port" "$terminals" "$environment" "$broker" "$user" "$app" "$front" "$market_front"
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
    CHANGED=1
    echo "Saved $file"
}

# apply_account: starts INSTANCE with what was saved, now or after trading hours.
apply_account() {
    systemctl enable "asterion-trading@$INSTANCE.service" >/dev/null 2>&1
    restart_after_trading "asterion-trading@$INSTANCE.service" "$RESTART-$INSTANCE"
    echo "A restart ends authorization: authorize $INSTANCE again in Terminal."
}

add_account() {
    INSTANCE=$(ask "Account name (as the Terminal names its instance)" "" "$ACCOUNT_NAME")
    if [[ -f $(account_file) ]]; then
        account_menu
        return
    fi
    CHANGED=
    configure_account
    [[ -n $CHANGED ]] || return 0
    echo "In Terminal, give account $INSTANCE this host's address, port $(current port) and certificate fingerprint."
    firewall "$(current port)"
    apply_account
}

# Whether the account menu stopped and disabled its account.
DISABLED=

disable_account() {
    confirm "Stop $INSTANCE now? Working orders stay at the broker and its authorization ends." n || return 0
    cancel_restart "$RESTART-$INSTANCE"
    systemctl disable --now "asterion-trading@$INSTANCE.service" >/dev/null 2>&1
    DISABLED=1
    echo "Stopped and disabled $INSTANCE. Its configuration and its trading record in $ROOT/data/$INSTANCE are kept."
}

# account_menu: INSTANCE's choices; saved changes take effect when it ends.
account_menu() {
    local choice
    CHANGED='' DISABLED=''
    while true; do
        echo
        echo "Account $INSTANCE  $(current bind):$(current port)"
        echo "  Service      $(status "asterion-trading@$INSTANCE.service" "$RESTART-$INSTANCE")"
        [[ -z $CHANGED ]] || echo "  Pending      a restart on Back applies the saved changes"
        echo "  Terminals"
        # shellcheck disable=SC2046
        numbered $(current terminals)
        echo
        echo "  1) Add Terminals"
        echo "  2) Remove Terminals"
        echo "  3) Change settings"
        echo "  4) Stop and disable"
        echo "  b) Back"
        read -r -p "Choose [b]: " choice || break
        case ${choice:-b} in
            1) edit_pins add "$(account_file)" terminals ;;
            2) edit_pins remove "$(account_file)" terminals ;;
            3) configure_account ;;
            4)
                disable_account
                if [[ -n $DISABLED ]]; then return; fi ;;
            b) break ;;
            *) echo "  Unknown choice" >&2 ;;
        esac
    done
    if [[ -n $CHANGED ]]; then apply_account; fi
}

# systemd remains the owner of runtime, boot and pending-restart state.
list_accounts() {
    local name unit state enabled pending i=0
    printf '    %-20s %-24s %-14s %-14s %s\n' "ACCOUNT" "LISTEN" "STATE" "AUTOSTART" "RESTART"
    for name in $(accounts); do
        i=$((i + 1))
        unit="asterion-trading@$name.service"
        state=$(systemctl show "$unit" --property=ActiveState --value)
        enabled=$(systemctl is-enabled "$unit" 2>/dev/null) || true
        pending=none
        if systemctl is-active --quiet "$RESTART-$name.timer"; then
            pending=$(systemctl show "$RESTART-$name.timer" --property=NextElapseUSecRealtime --value)
        fi
        printf '%2d) %-20s %-24s %-14s %-14s %s\n' "$i" "$name" "$(setting "$(account_file "$name")" bind ""):$(setting "$(account_file "$name")" port "")" "${state:-unknown}" "${enabled:-unknown}" "${pending:-scheduled}"
    done
}

menu() {
    local choice names
    while true; do
        echo
        echo "Asterion Trading  $ROOT"
        echo "  Certificate  $(certificate)"
        echo
        list_accounts
        mapfile -t names < <(accounts)
        echo
        ((${#names[@]} == 0)) || echo "  1-${#names[@]}) Open that account"
        echo "  a) Add an account"
        echo "  q) Done"
        read -r -p "Choose [q]: " choice || break
        case ${choice:-q} in
            a) add_account ;;
            q) break ;;
            *)
                if [[ $choice =~ ^[0-9]{1,3}$ ]] && ((choice >= 1 && choice <= ${#names[@]})); then
                    INSTANCE=${names[choice - 1]}
                    account_menu
                else
                    echo "  Unknown choice" >&2
                fi ;;
        esac
    done
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
            add_account
        fi
    fi
}
