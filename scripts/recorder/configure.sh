#!/bin/bash
# Sets up the recorder interactively: listening address and port and the
# counter go into config/recorder.json, which is all the recorder reads; the
# password and the authentication code are encrypted with systemd-creds,
# bound to this host, and never written to a file in the clear.
#
#   sudo /opt/asterion-recorder/bin/configure.sh
#
# Each question offers the current value as its default; press Enter to keep
# it. The password and the authentication code have no default. Then the
# recorder restarts, which is refused during trading hours.
set -euo pipefail

ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
CONFIG="$ROOT/config/recorder.json"
UNIT=asterion-recorder.service
ACCOUNT=asterion-recorder
CREDENTIALS=/etc/credstore.encrypted

fail() {
    echo "configure: $*" >&2
    exit 1
}

[[ $EUID -eq 0 ]] || fail "请用 sudo 运行"
[[ -f $CONFIG ]] || fail "找不到配置文件 $CONFIG；先运行 recording-host.py install"
python3 -I "$ROOT/bin/recording-host.py" trading-hours || exit 1

# The current value of a key ("port", "counter.broker") in the configuration.
current() {
    python3 -I -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    value = value.get(key, "") if isinstance(value, dict) else ""
print(value)' "$CONFIG" "$1"
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

front='^tcp://[A-Za-z0-9.-]+:[0-9]{1,5}$'
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
auth_code=
[[ -n $app_id ]] && auth_code=$(secret "授权码（不显示）")

# The settings file, replaced whole; only the recorder's account may read it.
temporary="$CONFIG.new"
python3 -I -c '
import json, sys
bind, port, market, trade, broker, user, app_id = sys.argv[2:]
settings = {"bind": bind, "port": int(port), "counter": {
    "market_front": market, "trade_front": trade, "broker": broker, "user": user, "app_id": app_id}}
with open(sys.argv[1], "w") as output:
    json.dump(settings, output, ensure_ascii=False, indent=2)
    output.write("\n")' "$temporary" "$bind" "$port" "$market_front" "$trade_front" "$broker" "$user" "$app_id"
chown "root:$ACCOUNT" "$temporary"
chmod 0640 "$temporary"
mv "$temporary" "$CONFIG"

printf '%s' "$password" | systemd-creds encrypt --name=asterion.ctp.password - "$CREDENTIALS/asterion.ctp.password" >/dev/null
if [[ -n $auth_code ]]; then
    printf '%s' "$auth_code" | systemd-creds encrypt --name=asterion.ctp.auth_code - "$CREDENTIALS/asterion.ctp.auth_code" >/dev/null
fi
unset password auth_code

systemctl restart "$UNIT"
echo "已保存 $CONFIG 并重启录制程序。查看状态：systemctl status $UNIT；日志在 $ROOT/logs"
