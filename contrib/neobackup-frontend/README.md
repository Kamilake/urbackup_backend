# NeoBackup Ransom Defender — 프론트엔드

UrBackup 서버 Web API 위에서 동작하는 React(DC) 기반 대체 웹 콘솔.
물리 확인 게이트(GPIO 17) 상태를 우아하게 표시하는 데 초점을 둔 스킨이다.

## 파일

| 파일 | 설명 |
|------|------|
| `NeoBackup.dc.html` | 소스(템플릿 + 로직). `support.js` 런타임 필요. 실서버 연동 구현 포함 |
| `support.js` | DC 런타임(`dc-runtime/src/*.ts`에서 생성, 수정 금지) |
| `deploy.sh` | `/usr/share/urbackup/www`에 `neo.html`로 배포 |
| `MIGRATION.md` | 데모→실API 마이그레이션 가이드(원본) |

## 배포

```bash
./deploy.sh
# 접속: http://<server>:55414/neo.html
```

## 동작 모드

- 기본은 **실서버 모드**(`this.DEMO=false`) — `/x?a=...` 같은 출처 호출.
- URL 파라미터로 강제 전환: `?demo=1`(목업) / `?demo=0`(실서버).

## 인증

- 웹 사용자가 없는 서버: 사용자명/비밀번호를 **비우고 로그인** → 익명 관리자.
- 사용자가 있으면: salt → PBKDF2/MD5 → login 2단계 챌린지·응답(crypto-js).

## 게이트(핵심)

파일 백업 "즉시 삭제"(`delete_now`, 양수 backupid)만 물리 버튼 승인 대상.
요청이 최대 310초 블로킹되며, GPIO 버튼을 누르면 삭제, 타임아웃이면
`physical_confirmation_timeout`으로 안전하게 보존된다.

## API 레퍼런스

- 일반 API: `../../docs/web-api.md`
- 게이트 동작: `../../docs/web-api-neobackup-gate.md`
- 게이트 내부/빌드: `../../docs/physical-gate.md`
