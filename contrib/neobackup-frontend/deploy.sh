#!/usr/bin/env bash
# NeoBackup Ransom Defender 프론트엔드 배포 스크립트
# 같은 출처(same-origin)로 서빙: http://<server>:55414/neo.html
#
# 사용법:
#   ./deploy.sh            # neo.html + support.js 를 UrBackup www 에 배포
#
# 주의(AGENTS.md §6/§8):
#  - /usr/share/urbackup 는 root:root 755/644 여야 웹 UI 가 404 나지 않음.
#  - apt 로 urbackup-server 가 갱신되면 이 파일들이 덮어써짐 → 갱신 후 재실행.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WWW_DIR="/usr/share/urbackup/www"

echo "[1/3] 대상 확인: $WWW_DIR"
[ -d "$WWW_DIR" ] || { echo "ERROR: $WWW_DIR 없음 (urbackup-server 설치 확인)"; exit 1; }

echo "[2/3] 배포: NeoBackup.dc.html -> neo.html, support.js"
sudo install -m644 -o root -g root "$SRC_DIR/NeoBackup.dc.html" "$WWW_DIR/neo.html"
sudo install -m644 -o root -g root "$SRC_DIR/support.js"        "$WWW_DIR/support.js"

echo "[3/3] 검증"
code=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:55414/neo.html || echo "000")
echo "  http://localhost:55414/neo.html -> HTTP $code"
[ "$code" = "200" ] && echo "✅ 배포 완료" || echo "⚠️ HTTP $code (서버 기동/권한 확인)"
