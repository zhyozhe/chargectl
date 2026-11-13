#!/bin/sh
# chargectl の導入スクリプト。端末の root シェルで実行する。
# バイナリ (chargectl) と chargectl.conf を、このスクリプトと同じ場所から配置する。
set -e
cd "$(dirname "$0")"

if [ "$(id -u)" != "0" ]; then
  echo "root で実行してください" >&2
  exit 1
fi

install -m 755 chargectl /usr/local/bin/chargectl
install -m 644 chargectl.conf /etc/init/chargectl.conf
mkdir -p /usr/local/share/chargectl

# 既に動いていれば再起動、止まっていれば起動
if status chargectl 2>/dev/null | grep -q "start/running"; then
  restart chargectl
else
  start chargectl
fi

echo "導入しました。端末の Chrome で http://127.0.0.1:8765/ を開いてください。"
