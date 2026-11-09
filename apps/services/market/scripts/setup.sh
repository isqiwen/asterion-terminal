#!/bin/bash
# Installs, upgrades and configures Market on a service host.
#
#   sudo bash Asterion-Market-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/market/bin/setup.sh   change the settings
#   sudo /opt/asterion/market/bin/setup.sh fingerprint   show this host's certificate fingerprint
#
# The release, Asterion-Market-VERSION-linux-x86_64.sh, is this script followed
# by apps/services/common/scripts/setup.sh, with the installation appended as
# a tar.gz after a line "__ARCHIVE__" (built by the CMake target
# asterion_market_setup). The installed copy, bin/setup.sh, carries nothing
# and only changes the settings.
#
# The installation is /opt/asterion/market: bin/ lib/ config/ logs/, and
# data/ unless the settings name another data directory.
# A first install asks for the settings; an upgrade replaces only bin/ and
# lib/ and leaves config/ (market.json, logging.json) as it is
# (it offers to change the settings).
# Settings questions offer the current values as defaults; the password and
# the authentication code are encrypted with systemd-creds, never written to
# a file in the clear. Market runs as the system service asterion-market
# under the account that ran the install with sudo. While it serves during
# trading hours, a change takes effect when the trading window ends.
set -euo pipefail

NAME=Market
PACKAGE=asterion-market
ROOT=/opt/asterion/market
UNIT=asterion-market.service
PROGRAMS=(bin/asterion_market bin/asterion_crash_reporter bin/setup.sh lib/ctp-md.so lib/ctp-trader.so)

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

python() {
    python3 "$@"
}

# The current value of a key ("port", "counter.broker"), or its default; the
# data directory defaults to one beside the programs.
current() {
    local default
    if [[ $1 == data ]]; then default=$ROOT/data; else default=${DEFAULTS[$1]}; fi
    setting "$ROOT/config/market.json" "$1" "$default"
}

configure() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' bind port data previous clients
    local market_front trade_front broker user app_id
    local password='' auth_code='' settings encrypted_password='' encrypted_auth_code=''
    bind=$(ask "Listen address" "$(current bind)" '^[0-9A-Fa-f.:]+$')
    port=$(ask "Listen port" "$(current port)" '^[0-9]{1,5}$')
    ((port >= 1 && port <= 65535)) || fail "The port must be between 1 and 65535"
    previous=$(current data)
    data=$(ask "Data directory" "$previous" '^/[^[:space:]]*$')
    clients=$(ask "Terminal fingerprints (space separated)" "$(current clients)" '^[0-9A-Fa-f:[:space:]]+$')
    clients=$(pins "$clients")
    echo "Use a CTP account of its own for quotes, never a live trading account."
    market_front=$(ask "CTP market front" "$(current counter.market_front)" "$front")
    trade_front=$(ask "CTP trading front" "$(current counter.trade_front)" "$front")
    broker=$(ask "CTP broker ID" "$(current counter.broker)" '^[A-Za-z0-9]+$')
    user=$(ask "CTP investor ID" "$(current counter.user)" '^[A-Za-z0-9]+$')
    app_id=$(ask "CTP AppID (- for none)" "$(current counter.app_id)" '^[A-Za-z0-9_-]*$')
    [[ $app_id == - ]] && app_id=
    # Credentials of the same counter account may be kept; another account needs its own.
    if ! [[ -f $ROOT/config/market.json && $broker == "$(current counter.broker)"
        && $user == "$(current counter.user)" && $app_id == "$(current counter.app_id)"
        && -f $CREDENTIALS/asterion.ctp.password && (-z $app_id || -f $CREDENTIALS/asterion.ctp.auth_code) ]] ||
        ! confirm "Keep the saved CTP password and authentication code?" y; then
        password=$(secret "CTP password (not shown)")
        [[ -z $app_id ]] || auth_code=$(secret "CTP authentication code (not shown)")
    fi

    # Everything is asked; now the files, each staged and moved into place.
    stage settings "$ROOT/config/market.json"
    python -I -c '
import json, sys
path, bind, port, data, market, trade, broker, user, app_id, clients = sys.argv[1:]
settings = {"bind": bind, "port": int(port), "data": data,
    "clients": [{"fingerprint": pin} for pin in clients.split()],
    "counter": {"market_front": market, "trade_front": trade, "broker": broker, "user": user, "app_id": app_id}}
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$settings" "$bind" "$port" "$data" "$market_front" "$trade_front" \
        "$broker" "$user" "$app_id" "$clients"
    private "$settings"
    [[ -z $password ]] || seal encrypted_password "$CREDENTIALS/asterion.ctp.password" asterion.ctp.password "$password"
    [[ -z $auth_code ]] || seal encrypted_auth_code "$CREDENTIALS/asterion.ctp.auth_code" asterion.ctp.auth_code "$auth_code"
    password='' auth_code=''
    [[ -z $encrypted_password ]] || mv -f "$encrypted_password" "$CREDENTIALS/asterion.ctp.password"
    [[ -z $encrypted_auth_code ]] || mv -f "$encrypted_auth_code" "$CREDENTIALS/asterion.ctp.auth_code"
    data_directory "$previous" "$data"
    mv -f "$settings" "$ROOT/config/market.json"
    echo "Saved $ROOT/config/market.json"
    # The service may write only its logs and its data directory.
    write_unit
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

setup() {
    install_or_configure "$1"
    echo "Registered clients take effect after restart. Credentials stay on this host."
    restart_after_trading "$UNIT" asterion-market-restart
}
