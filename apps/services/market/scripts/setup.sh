#!/bin/bash
# 在服务主机上安装、升级与管理 Market：
#
#   sudo bash Asterion-Market-VERSION-linux-x86_64.sh   首次安装或升级
#   sudo asterion-market                                打开管理面板（或 sudo /opt/asterion/market/bin/setup.sh）
#   sudo asterion-market fingerprint                    查看本机证书指纹
set -euo pipefail

NAME=Market
PACKAGE=asterion-market
ROOT=/opt/asterion/market
UNIT=asterion-market.service
SETTINGS=config/market.json
TIMER=asterion-market-restart
PROGRAMS=(bin/asterion_market bin/asterion_crash_reporter bin/setup.sh bin/cold_backup.py lib/ctp-md.so lib/ctp-trader.so)

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

current() {
    local default
    if [[ $1 == data ]]; then default=$ROOT/data; else default=${DEFAULTS[$1]}; fi
    setting "$ROOT/$SETTINGS" "$1" "$default"
}

write_settings() {
    python -I -c '
import json, sys
path, bind, port, data, market, trade, broker, user, app_id, clients = sys.argv[1:]
settings = {"bind": bind, "port": int(port), "data": data,
    "clients": [{"fingerprint": pin} for pin in clients.split()],
    "counter": {"market_front": market, "trade_front": trade, "broker": broker, "user": user, "app_id": app_id}}
with open(path, "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$1" "$bind" "$port" "$data" "$market_front" "$trade_front" \
        "$broker" "$user" "$app_id" "$clients"
}

current_settings() {
    local bind port data clients market_front trade_front broker user app_id
    bind=$(current bind) port=$(current port) data=$(current data) clients=$(current clients)
    market_front=$(current counter.market_front) trade_front=$(current counter.trade_front)
    broker=$(current counter.broker) user=$(current counter.user) app_id=$(current counter.app_id)
    write_settings "$1"
}

commit_market_settings() {
    local previous=$1 password=$2 auth_code=$3 settings encrypted_password='' encrypted_auth_code=''
    stage settings "$ROOT/$SETTINGS"
    write_settings "$settings"
    private "$settings"
    [[ -z $password ]] || seal encrypted_password "$CREDENTIALS/asterion.ctp.password" asterion.ctp.password "$password"
    [[ -z $auth_code ]] || seal encrypted_auth_code "$CREDENTIALS/asterion.ctp.auth_code" asterion.ctp.auth_code "$auth_code"
    [[ -z $encrypted_password ]] || mv -f "$encrypted_password" "$CREDENTIALS/asterion.ctp.password"
    if [[ -z $app_id ]]; then
        rm -f "$CREDENTIALS/asterion.ctp.auth_code"
    elif [[ -n $encrypted_auth_code ]]; then
        mv -f "$encrypted_auth_code" "$CREDENTIALS/asterion.ctp.auth_code"
    fi
    data_directory "$previous" "$data"
    mv -f "$settings" "$ROOT/$SETTINGS"
    CHANGED=1
    echo "已保存 $ROOT/$SETTINGS"
    write_unit
}

configure() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' bind port data previous clients
    local market_front trade_front broker user app_id password='' auth_code=''
    CHANGES=()
    field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
    number_field port "监听端口" port 1 65535
    previous=$(current data)
    field data "数据缓存目录" data '^/[^[:space:]]*$'
    echo "请使用独立的行情账号，切勿配置实盘交易账户。"
    field market_front "CTP 行情前置" counter.market_front "$front"
    field trade_front "CTP 交易前置" counter.trade_front "$front"
    field broker "CTP 经纪商代码" counter.broker '^[A-Za-z0-9]+$'
    field user "CTP 投资者账号" counter.user '^[A-Za-z0-9]+$'
    app_id=$(ask "CTP AppID（填 - 表示无）" "$(current counter.app_id)" '^[A-Za-z0-9_-]*$')
    [[ $app_id != - ]] || app_id=
    note "CTP AppID" "$(current counter.app_id)" "$app_id"
    clients=$(current clients)
    [[ -f $ROOT/$SETTINGS ]] || clients=$(add_pins)
    if ! [[ -f $ROOT/$SETTINGS && $broker == "$(current counter.broker)"
        && $user == "$(current counter.user)" && $app_id == "$(current counter.app_id)"
        && -f $CREDENTIALS/asterion.ctp.password && (-z $app_id || -f $CREDENTIALS/asterion.ctp.auth_code) ]] ||
        ! confirm "保留已保存的 CTP 密码与授权码？" y; then
        password=$(secret "CTP 密码（输入不回显）")
        [[ -z $app_id ]] || auth_code=$(secret "CTP 授权码（输入不回显）")
        CHANGES+=("CTP 密码与授权码: 已更新")
    fi
    if [[ -f $ROOT/$SETTINGS ]] && ! review; then return; fi
    commit_market_settings "$previous" "$password" "$auth_code"
}

settings_menu() {
    local front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$' choice
    local bind port data previous clients market_front trade_front broker user app_id password auth_code
    local encrypted_password encrypted_auth_code
    while true; do
        bind=$(current bind) port=$(current port) data=$(current data) clients=$(current clients)
        market_front=$(current counter.market_front) trade_front=$(current counter.trade_front)
        broker=$(current counter.broker) user=$(current counter.user) app_id=$(current counter.app_id)
        echo
        hr_single
        echo " ${C_BOLD}配置与凭据设置（Market）${C_RESET}"
        hr_single
        echo " 1) 监听地址与端口        $bind:$port"
        echo " 2) 数据缓存目录          $data"
        echo " 3) CTP 行情与交易前置    $market_front / $trade_front"
        echo " 4) CTP 账号与 AppID      $broker / $user / ${app_id:-无}"
        echo " 5) ${C_YELLOW}更新 CTP 密码与授权码${C_RESET} $([[ -f $CREDENTIALS/asterion.ctp.password ]] && echo 已加密保存 || echo 未设置)"
        echo " 6) ${C_CYAN}逐项向导修改全部${C_RESET}"
        echo " 0) 返回上级菜单"
        hr_single
        read -r -p "请输入对应数字 [0]> " choice || break
        CHANGES=()
        previous=$data
        password='' auth_code=''
        case ${choice:-0} in
            1)
                field bind "监听地址" bind '^[0-9A-Fa-f.:]+$'
                number_field port "监听端口" port 1 65535
                review && commit_market_settings "$previous" "" "" ;;
            2)
                field data "数据缓存目录" data '^/[^[:space:]]*$'
                review && commit_market_settings "$previous" "" "" ;;
            3)
                field market_front "CTP 行情前置" counter.market_front "$front"
                field trade_front "CTP 交易前置" counter.trade_front "$front"
                review && commit_market_settings "$previous" "" "" ;;
            4)
                echo "请使用独立的行情账号，切勿配置实盘交易账户。"
                field broker "CTP 经纪商代码" counter.broker '^[A-Za-z0-9]+$'
                field user "CTP 投资者账号" counter.user '^[A-Za-z0-9]+$'
                app_id=$(ask "CTP AppID（填 - 表示无）" "$(current counter.app_id)" '^[A-Za-z0-9_-]*$')
                [[ $app_id != - ]] || app_id=
                note "CTP AppID" "$(current counter.app_id)" "$app_id"
                if ! [[ $broker == "$(current counter.broker)" && $user == "$(current counter.user)" && $app_id == "$(current counter.app_id)"
                    && -f $CREDENTIALS/asterion.ctp.password && (-z $app_id || -f $CREDENTIALS/asterion.ctp.auth_code) ]] ||
                    ! confirm "保留已保存的 CTP 密码与授权码？" y; then
                    password=$(secret "CTP 密码（输入不回显）")
                    [[ -z $app_id ]] || auth_code=$(secret "CTP 授权码（输入不回显）")
                    CHANGES+=("CTP 密码与授权码: 已更新")
                fi
                review && commit_market_settings "$previous" "$password" "$auth_code" ;;
            5)
                password=$(secret "CTP 密码（输入不回显）")
                [[ -z $app_id ]] || auth_code=$(secret "CTP 授权码（输入不回显）")
                CHANGES+=("CTP 密码与授权码: 已更新")
                if review; then
                    encrypted_password='' encrypted_auth_code=''
                    seal encrypted_password "$CREDENTIALS/asterion.ctp.password" asterion.ctp.password "$password"
                    [[ -z $auth_code ]] || seal encrypted_auth_code "$CREDENTIALS/asterion.ctp.auth_code" asterion.ctp.auth_code "$auth_code"
                    password='' auth_code=''
                    mv -f "$encrypted_password" "$CREDENTIALS/asterion.ctp.password"
                    if [[ -z $app_id ]]; then
                        rm -f "$CREDENTIALS/asterion.ctp.auth_code"
                    elif [[ -n $encrypted_auth_code ]]; then
                        mv -f "$encrypted_auth_code" "$CREDENTIALS/asterion.ctp.auth_code"
                    fi
                    CHANGED=1
                    echo "已加密保存 CTP 密码与授权码"
                fi ;;
            6) configure ;;
            0) break ;;
            *) echo "  无效选项，请重新输入" >&2 ;;
        esac
    done
}

unit_text() {
    cat <<EOF
[Unit]
Description=Asterion live quotes
Wants=network-online.target time-sync.target
After=network-online.target time-sync.target
StartLimitIntervalSec=0

[Service]
User=$ACCOUNT
UMask=0077
ExecStart=$ROOT/bin/asterion_market
ReadWritePaths=$ROOT/logs $(current data)
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

apply() {
    restart_after_trading "$UNIT" "$TIMER"
}

setup() {
    install_or_configure "$1"
}
