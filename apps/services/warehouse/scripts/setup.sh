#!/bin/bash
# Installs, upgrades and configures Warehouse on a service host.
#
#   sudo bash Asterion-Warehouse-VERSION-linux-x86_64.sh       install, or upgrade an installation
#   sudo /opt/asterion/warehouse/bin/setup.sh   add or remove Terminals, change the settings
#   sudo /opt/asterion/warehouse/bin/setup.sh fingerprint   show this host's certificate fingerprint
#
# The release, Asterion-Warehouse-VERSION-linux-x86_64.sh, is this script
# followed by apps/services/common/scripts/setup.sh, with the installation
# appended as a tar.gz after a line "__ARCHIVE__" (built by the CMake target
# asterion_warehouse_setup). The installed copy, bin/setup.sh, carries nothing
# and only opens the menu.
#
# The installation is /opt/asterion/warehouse: bin/ config/ logs/, and
# data/ unless the settings name another data directory.
# A first install asks for the settings; an upgrade replaces only bin/,
# writes warehouse.json in this release's format (current values kept, new
# keys at their defaults, keys no longer read dropped, each change shown),
# keeps logging.json and opens the menu.
# Settings questions offer the current values; a change is shown and saved
# only when confirmed. The Tushare token is encrypted with systemd-creds,
# never written to a file in the clear. Warehouse runs as the system service
# asterion-warehouse under the account that ran the install with sudo; saved
# changes take effect together, with one restart when the run ends.
set -euo pipefail

NAME=Warehouse
PACKAGE=asterion-warehouse
ROOT=/opt/asterion/warehouse
UNIT=asterion-warehouse.service
SETTINGS=config/warehouse.json
PROGRAMS=(bin/asterion_warehouse bin/asterion_crash_reporter bin/setup.sh)

# Defaults for new configurations only; existing instances keep their own values.
declare -Ar DEFAULTS=(
    [bind]='0.0.0.0'
    [port]='7450'
    [clients]=''
    [acquisition.exchanges]='SHFE INE CZCE DCE CFFEX GFEX'
    [acquisition.tushare.requests_per_minute]='500'
    [acquisition.update_at]='06:00'
)

python() {
    python3 "$@"
}

# The current value of a key ("port", "acquisition.exchanges"), or its
# default; the data directory defaults to one beside the programs.
current() {
    local default
    if [[ $1 == data ]]; then default=$ROOT/data; else default=${DEFAULTS[$1]}; fi
    setting "$ROOT/$SETTINGS" "$1" "$default"
}

EXCHANGES=" SHFE INE CZCE DCE CFFEX GFEX "

# ask_exchanges DEFAULT: distinct exchanges from EXCHANGES, or "-" for none.
ask_exchanges() {
    local value exchange picked valid
    while true; do
        value=$(ask "Public exchanges (- for none)" "$1" '^[A-Z -]+$')
        if [[ $value == - ]]; then
            printf -- -
            return
        fi
        picked=() valid=1
        for exchange in $value; do
            [[ $EXCHANGES == *" $exchange "* && " ${picked[*]} " != *" $exchange "* ]] || valid=
            picked+=("$exchange")
        done
        if [[ -n $valid && ${#picked[@]} -gt 0 ]]; then
            printf '%s' "${picked[*]}"
            return
        fi
        echo "  Choose distinct ones from:$EXCHANGES" >&2
    done
}

# write_settings PATH: the settings file from bind, port, data, clients,
# exchanges, budget and update_at ("-" for off) of the caller.
write_settings() {
    python -I -c '
import json, sys
path, bind, port, data, clients, exchanges, budget, update_at = sys.argv[1:]
settings = {"bind": bind, "port": int(port), "data": data,
    "clients": [{"fingerprint": pin} for pin in clients.split()],
    "acquisition": {"exchanges": exchanges.split(), "tushare": {"requests_per_minute": int(budget)},
        "update_at": None if update_at == "-" else update_at}}
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$1" "$bind" "$port" "$data" "$clients" "$exchanges" "$budget" "$update_at"
}

# current_settings PATH: the settings file with the current values.
current_settings() {
    local bind port data clients exchanges budget update_at
    bind=$(current bind) port=$(current port) data=$(current data) clients=$(current clients)
    exchanges=$(current acquisition.exchanges) budget=$(current acquisition.tushare.requests_per_minute)
    update_at=$(current acquisition.update_at)
    update_at=${update_at:--}
    write_settings "$1"
}

# configure: asks for the settings other than the Terminals (those too on
# install) and saves what changed.
configure() {
    local bind port data previous settings clients exchanges budget update_at previous_update token='' encrypted_token=''
    CHANGES=()
    field bind "Listen address" bind '^[0-9A-Fa-f.:]+$'
    number_field port "Listen port" port 1 65535
    previous=$(current data)
    field data "Data directory" data '^/[^[:space:]]*$'
    exchanges=$(current acquisition.exchanges)
    exchanges=$(ask_exchanges "${exchanges:--}")
    [[ $exchanges != - ]] || exchanges=
    note "Public exchanges" "$(current acquisition.exchanges)" "$exchanges"
    number_field budget "Tushare requests per minute" acquisition.tushare.requests_per_minute 1 500
    # Saved as null when off.
    previous_update=$(current acquisition.update_at)
    previous_update=${previous_update:--}
    update_at=$(ask "Daily update, Beijing time (HH:MM, - for off)" "$previous_update" '^(([01][0-9]|2[0-3]):[0-5][0-9]|-)$')
    note "Daily update" "$previous_update" "$update_at"
    clients=$(current clients)
    [[ -f $ROOT/$SETTINGS ]] || clients=$(add_pins)
    if ! [[ -f $CREDENTIALS/asterion.warehouse.tushare.token ]] || ! confirm "Keep the saved Tushare token?" y; then
        while true; do
            token=$(secret "Tushare token (not shown)")
            [[ ${#token} -gt 256 || $token =~ [[:space:]] ]] || break
            echo "  At most 256 characters without whitespace" >&2
        done
        CHANGES+=("Tushare token: replaced")
    fi
    if [[ -f $ROOT/$SETTINGS ]] && ! review; then return; fi

    # Everything is asked; now the files, each staged and moved into place.
    stage settings "$ROOT/$SETTINGS"
    write_settings "$settings"
    private "$settings"
    [[ -z $token ]] || seal encrypted_token "$CREDENTIALS/asterion.warehouse.tushare.token" asterion.warehouse.tushare.token "$token"
    token=''
    [[ -z $encrypted_token ]] || mv -f "$encrypted_token" "$CREDENTIALS/asterion.warehouse.tushare.token"
    data_directory "$previous" "$data"
    mv -f "$settings" "$ROOT/$SETTINGS"
    CHANGED=1
    echo "Saved $ROOT/$SETTINGS"
    # The service may write only its logs and its data directory.
    write_unit
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion historical data
Wants=network-online.target
After=network-online.target
# Never give up restarting: a service that stays down serves nothing.
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
# It finds config/ and logs/ beside its bin/; config/warehouse.json names its data.
ExecStart=$ROOT/bin/asterion_warehouse
ReadWritePaths=$ROOT/logs $(current data)
# The Tushare token, decrypted at start.
ImportCredential=asterion.warehouse.*
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

apply() {
    systemctl restart "$UNIT"
    echo "Restarted $UNIT. Logs in $ROOT/logs"
}

setup() {
    install_or_configure "$1"
}
