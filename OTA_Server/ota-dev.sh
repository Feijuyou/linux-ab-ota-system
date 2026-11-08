
#!/bin/bash

set -e

cd "$HOME/OTA_Server"

echo "=== 启动 OTA 开发环境 ==="

# 1. 检查 HTTP 服务是否已启动
if ! curl -fsS http://127.0.0.1:8000/manifest.json > /dev/null; then
    echo "[1] 启动 HTTP Server..."

    nohup python3 -m http.server 8000 \
        --bind 127.0.0.1 \
        > /tmp/ota-http.log 2>&1 < /dev/null &

    sleep 1
else
    echo "[1] HTTP Server 已运行"
fi

# 2. 确认 HTTP 服务可用
curl -fsS http://127.0.0.1:8000/manifest.json > /dev/null

echo "[2] HTTP Server 正常"
echo "[3] 建立 SSH 隧道并登录 RDK X3..."

# 3. 建立隧道并进入开发板
ssh -o ExitOnForwardFailure=yes \
    -R 18000:127.0.0.1:8000 \
    sunrise@192.168.127.10
