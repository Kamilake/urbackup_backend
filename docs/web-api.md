# UrBackup Server Web API 레퍼런스 (프론트엔드 작성용)

이 문서 하나만 있으면 백엔드 코드를 보지 않고도 UrBackup 서버용 프론트엔드(React 등)를
작성할 수 있도록 핵심 엔드포인트·인증·액션별 스키마를 정리한 것이다. UI 표시와 상태 읽기,
백업 조회·복원에 필요한 액션을 대부분 포함한다.

> 근거 코드: `urbackupserver/serverinterface/*.cpp`. 변경 시 해당 파일과 대조할 것.

---

## 1. 전송 규약 (모든 액션 공통)

UrBackup은 REST가 아니라 **단일 "action" 디스패치** 방식이다.

- **메서드**: `POST` (브라우저 다운로드/이미지처럼 GET이 가능한 일부 제외)
- **URL**: `<base>/x?a=<action>`
  - 웹 UI 기본 포트: HTTP **55414**. 예: `http://server:55414/x?a=status`
  - `a` 쿼리 파라미터가 호출할 액션 이름이다.
- **요청 본문 인코딩**: `application/x-www-form-urlencoded; charset=utf-8`
  - 즉 파라미터는 `key=value&key2=value2` 형태로 보낸다(JSON 본문 아님).
- **응답**: 항상 JSON (`Content-Type: application/json`).
- **세션**: 로그인 후 받은 세션 ID를 매 요청에 `ses=<session>` 파라미터로 첨부한다.
- **언어**(선택): `lang=en` 또는 `Accept-Language` 헤더로 결정.

### 공통 에러 응답
대부분의 액션은 다음 형태로 에러를 알린다.

| 응답 | 의미 | 프론트 처리 |
|------|------|-------------|
| `{"error": 1}` | 세션 없음/만료/무효 | 로그인 화면으로 보내고 재인증 |
| `{"error": 2}` | 인증 실패 또는 대상 없음 | 메시지 표시 |
| `{"error": 3}` | 로그인 rate limit | 잠시 후 재시도 안내 |
| `{"err": "access_denied", "errcode": "..."}` | 백업 토큰/권한 거부 (backups 액션) | 접근 거부 표시 |

> 권장 fetch 래퍼: 응답을 받으면 먼저 `data.error === 1`인지 검사해 세션 만료를 전역 처리하고,
> 그 외에는 액션별로 해석한다(레거시 `www/js/urbackup_functions.js`의 `getJSON`과 동일한 규약).

### 최소 fetch 예시 (TypeScript)
```ts
const BASE = ""; // 동일 출처라면 빈 문자열, 아니면 "http://server:55414"
let SESSION = "";

async function api<T = any>(action: string, params: Record<string, string> = {}): Promise<T> {
  const body = new URLSearchParams({ ...params });
  if (SESSION) body.set("ses", SESSION);
  const res = await fetch(`${BASE}/x?a=${action}`, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded; charset=utf-8" },
    body,
  });
  return res.json();
}
```

---

## 2. 인증

비밀번호는 평문으로 보내지 않는다(LDAP 제외). **salt → login** 2단계 챌린지-응답이다.

### 2.1 `salt` — 솔트/챌린지 요청 + 세션 생성
**요청 파라미터**

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| `username` | ✓ | 로그인할 사용자명 |

**응답** (`salt.cpp`)
```jsonc
{
  "ses": "AbCd...",          // 새로 생성된 세션 ID. 이후 모든 요청에 ses=로 첨부
  "salt": "...",             // 사용자 salt (사용자 없으면 더미 salt 반환 — 사용자 열거 방지)
  "pbkdf2_rounds": 10000,    // PBKDF2 반복 횟수 (0이면 PBKDF2 미사용)
  "rnd": "..."               // 이번 로그인용 1회성 난수(서버 챌린지)
}
// 실패: {"error": 0} 잘못된 사용자(열거 허용 시) / {"error": 1} 세션 생성 실패 / {"error": 3} rate limit
```

### 2.2 비밀번호 해시 계산 (클라이언트 측)
레거시 구현(`www/js/urbackup.js`)과 동일한 알고리즘:

```
pwmd5  = pbkdf2_rounds > 0
           ? PBKDF2(password, hexToBytes(salt), rounds=pbkdf2_rounds, keylen=32, SHA256)  // hex 문자열
           : MD5(salt + password)
password_field = MD5(rnd + pwmd5)   // 최종적으로 login에 보낼 값
```
- crypto-js 사용 예: `PBKDF2(password, hexSalt, { keySize: 256/32, iterations: rounds, hasher: SHA256 }).toString()`
  뒤에 `MD5(rnd + pwmd5).toString()`.

### 2.3 `login` — 로그인 수행
**요청 파라미터**

| 파라미터 | 필수 | 설명 |
|----------|------|------|
| `ses` | ✓ | `salt`에서 받은 세션 |
| `username` | ✓ | 사용자명 |
| `password` | ✓ | 위에서 계산한 `password_field` (LDAP면 평문 + `plainpw=1`) |
| `plainpw` | LDAP 시 | `1`이면 평문 비밀번호 전송(LDAP 전용, TLS 전제) |

**응답** (`login.cpp`)
```jsonc
{
  "success": true,
  "session": "AbCd...",      // 세션(없으면 기존 ses 유지)
  "api_version": 2,
  "lang": "en",
  // ↓ 도메인별 권한 문자열: "all" | "none" | "1,2,3"(허용된 clientid 목록)
  "status": "all",
  "graph": "all",
  "progress": "all",
  "browse_backups": "all",
  "settings": "all",
  "logs": "all"
}
```
실패/특수 응답:
```jsonc
{"success": false, "error": 2}     // 사용자명/비밀번호 틀림
{"success": false, "error": 1}     // 사용자가 아직 없음(최초 설정 상태)
{"success": false, "ldap_enabled": true}
{"success": false, "admin_only": "<name>"}   // admin 계정만 존재
// 서버 기동 중일 때(로그인 전 상태 폴링용):
{"creating_filescache": true, "processed_file_entries": N, "percent_finished": 12.3, "lang": "en"}
{"upgrading_database": true, "curr_db_version": 68, "target_db_version": 69, "lang": "en"}
```

> **권한 사용법**: `"all"`=전체, `"none"`=없음, `"1,2,3"`=해당 clientid만. 프론트는 이 값으로
> 메뉴/버튼을 게이팅한다. 예: `status !== "none"`이면 상태 페이지 표시.

### 익명 로그인
`username` 없이 `login`을 호출하면(사용자가 한 명도 없는 초기 서버) 익명 세션을 발급하고
`success: true`를 반환할 수 있다.

---

## 3. 상태 / 대시보드 읽기

### 3.1 `status` — 클라이언트 목록 및 상태 (메인 대시보드)
**권한**: `status`. **요청**: `ses` (+ 선택 액션 파라미터, 아래 4.x 참조).

**응답** (`status.cpp`)
```jsonc
{
  "has_status_check": true,       // 관리자(all)일 때
  "nospc_stalled": true,          // (선택) 저장공간 부족으로 정지
  "nospc_fatal": true,            // (선택)
  "database_error": true,         // (선택) DB 오류
  "status": [
    {
      "id": 1,
      "name": "DESKTOP-XYZ",
      "groupname": "",
      "online": true,
      "ip": "192.168.0.10",
      "lastseen": 1719300000,           // epoch초 ('-' 일 수 있음: 미등록 클라이언트)
      "lastbackup": 1719200000,         // 마지막 파일 백업 시각(epoch초, 0=없음)
      "lastbackup_image": 1719100000,   // 마지막 이미지 백업 시각
      "file_ok": true,                  // 최근 파일 백업 정상 여부
      "image_ok": true,
      "file_disabled": true,            // (선택) 파일 백업 비활성
      "image_disabled": true,           // (선택)
      "image_not_supported": true,      // (선택) 이미지 백업 미지원 OS
      "no_backup_paths": true,          // (선택) 백업 경로 미설정
      "last_filebackup_issues": 0,      // 0=문제없음, >0=경고 수
      "delete_pending": "0",
      "uid": "",
      "client_version_string": "2.5.x",
      "os_version_string": "Windows 10",
      "os_simple": "windows",           // windows|linux|osx|...
      "status": 2,                      // 현재 동작 코드(아래 표)
      "processes": [                    // 진행 중 작업 요약
        { "action": 2, "pcdone": 42 }
      ]
    }
  ]
}
```

**`status` 코드(클라이언트 현재 상태)**

| 코드 | 의미 |
|------|------|
| 0 | 유휴(idle) |
| 1 | 증분 파일 백업 |
| 2 | 전체 파일 백업 |
| 3 | 증분 이미지 백업 |
| 4 | 전체 이미지 백업 |
| 5 | 증분 파일 재개 |
| 6 | 전체 파일 재개 |
| 7 | CDP 동기화 |
| 8 | 파일 복원 |
| 9 | 이미지 복원 |
| 10 | 클라이언트 업데이트 |
| 11 | ident 오류 |
| 12 | 클라이언트 과다 |
| 13 | 인증 오류 |
| 14 | UID 변경됨 |
| 15 | 인증 중 |
| 16 | 설정 오류 |
| 17 | 시작 중 |

> `processes[].action`도 위 코드(0~17)와 동일한 액션 enum을 쓴다.

### 3.2 `progress` — 진행 중 작업 + 최근 활동 (자주 폴링)
**권한**: `progress`. **요청**: `ses`, (선택) `with_lastacts=0`으로 lastacts 생략 가능.

**응답** (`progress.cpp`)
```jsonc
{
  "progress": [
    {
      "name": "DESKTOP-XYZ",
      "clientid": 1,
      "action": 2,                  // 위 status 코드와 동일 enum
      "pcdone": 42,                 // 진행률 %
      "queue": 12,                  // 대기 큐 크기
      "id": 5,                      // 프로세스 id (중단 시 사용)
      "logid": 123,                 // 라이브 로그 id (livelog용, 0이면 없음)
      "details": "C:\\...",
      "total_bytes": 1048576,
      "done_bytes": 524288,
      "detail_pc": 30,
      "paused": false,
      "eta_ms": 60000,
      "speed_bpms": 1024.5,         // bytes/ms
      "past_speed_bpms": [1000, 1100],
      "can_stop_backup": true,      // (선택) 중단 권한 있을 때만
      "can_show_backup_log": true   // (선택) 로그 볼 권한 있을 때만
    }
  ],
  "lastacts": [ /* 3.3과 동일 구조 */ ]
}
```

**작업 중단**: 같은 `progress` 액션에 아래 파라미터를 함께 보낸다(권한 `stop_backup`).
- `stop_clientid=<clientid>`, `stop_id=<process id>`

### 3.3 `lastacts` — 최근 활동 로그 (최대 20개)
**권한**: `lastacts`. **요청**: `ses`.

**응답** (`lastacts.cpp`)
```jsonc
{
  "lastacts": [
    {
      "id": 42,                 // backup/restore id
      "clientid": 1,
      "name": "DESKTOP-XYZ",
      "backuptime": 1719200000, // epoch초
      "incremental": 1,         // 0=전체, >0=증분
      "duration": 3600,         // 초
      "size_bytes": 1048576,    // -1이면 미계산
      "image": 0,               // 1=이미지 백업
      "del": false,             // true=삭제 통계 항목
      "restore": 0,             // 1=복원 작업
      "resumed": 0,
      "details": "C:"           // 이미지면 드라이브 문자, 복원이면 경로 등
    }
  ]
}
```

### 3.4 `status_check` — 저장소 접근 점검 (관리자)
**권한**: `status`=all. **요청**: `ses`. 백업 폴더 쓰기/접근 가능 여부 등을 반환.

---

## 4. 백업 제어

### 4.1 `start_backup` — 백업 시작
**권한**: `start_backup`=all (그리고 대상 clientid에 `status` 권한 필요).
**요청 파라미터**

| 파라미터 | 설명 |
|----------|------|
| `ses` | 세션 |
| `start_client` | 시작할 clientid들. 쉼표 구분(`1,2,3`) |
| `start_type` | `full_file` \| `incr_file` \| `full_image` \| `incr_image` |

**응답** (`start_backup.cpp`)
```jsonc
{
  "result": [
    { "clientid": 1, "start_type": "incr_file", "start_ok": true }
  ]
}
```
`start_ok:false` = 클라이언트 오프라인이거나 타입이 잘못됨.

### 4.2 클라이언트 제거 / 표시 정리 (`status` 액션의 부가 기능)
`status` 액션에 아래 파라미터를 함께 보낸다(관리자 권한).

| 파라미터 | 효과 |
|----------|------|
| `remove_client=1,2` | 해당 clientid 삭제 예약(`delete_pending=1`) |
| `remove_client=1,2&stop_remove_client=1` | 삭제 예약 취소 |
| `reset_client_uid=<id>` | 클라이언트 UID 리셋 |
| `stop_show=<...>` | 미등록(extra) 클라이언트 숨김 |
| `reset_error=nospc_stalled\|nospc_fatal\|database_error` | 서버 에러 상태 해제 |

---

## 5. 백업 조회 · 파일 탐색 · 다운로드 (`backups` 액션)

`backups`는 `sa`(sub-action) 파라미터로 동작이 갈린다. 권한: `browse_backups`.
이 액션은 `ses`(POST) 또는 토큰 인증(아래 5.6)을 쓸 수 있다.

### 5.1 클라이언트 목록 (`sa` 없음)
**요청**: `ses` (sa 미지정)
**응답**
```jsonc
{
  "clients": [
    { "id": 1, "name": "DESKTOP-XYZ", "lastbackup": 1719200000 }
  ]
}
```
> 권한 대상 클라이언트가 정확히 1개면 서버가 자동으로 `sa=backups`로 전환해 바로 백업 목록을 준다.

### 5.2 특정 클라이언트의 백업 목록 — `sa=backups`
**요청**: `ses`, `clientid=<id>` (토큰 인증 시 `clientname=<name>`)
**응답** (`backups.cpp` / `get_backups_with_tokens`, `get_backup_images`)
```jsonc
{
  "clientname": "DESKTOP-XYZ",
  "clientid": 1,
  "can_archive": true,
  "can_delete": true,
  "backups": [                    // 파일 백업
    {
      "id": 42,
      "backuptime": 1719200000,   // epoch초
      "incremental": 1,           // 0=전체
      "size_bytes": 1048576,
      "archived": 0,              // 0=아님, !=0=수동 보관
      "delete_pending": true,     // (선택)
      "archive_timeout": 0        // (선택) 보관 만료까지 남은 초
    }
  ],
  "backup_images": [              // 이미지 백업
    {
      "id": 7,
      "backuptime": 1719100000,
      "incremental": 0,
      "size_bytes": 1073741824,
      "letter": "C:",             // 드라이브 문자
      "archived": 0,
      "delete_pending": true      // (선택)
    }
  ]
}
```
> **중요**: 파일 탐색/복원 시 **이미지 백업은 음수 backupid**로 표현한다(예: 이미지 id 7 → `backupid=-7`).
> 파일 백업은 양수.

### 5.3 백업 안 파일/디렉터리 탐색 — `sa=files`
**요청**: `ses`, `clientid`, `backupid=<id>`, `path=<백업 내부 경로>`
- 이미지 파일 탐색이면 `backupid`에 음수(이미지 id) 사용. `mount=1`로 이미지 마운트 후 탐색.

**응답**
```jsonc
{
  "clientname": "DESKTOP-XYZ",
  "clientid": 1,
  "backupid": 42,
  "path": "/Documents",
  "files": [
    {
      "name": "report.pdf",
      "dir": false,             // true=디렉터리
      "size": 12345,            // 디렉터리는 보통 생략/0
      "mod": 1719200000,        // 수정 시각(epoch초)
      "creat": 1719100000,      // 생성 시각
      "access": 1719200000      // 접근 시각
    }
  ]
}
```
이미지 마운트 결과에는 `can_mount: true` / `backupid` 등이 함께 올 수 있다.

### 5.4 단일 파일 다운로드 — `sa=filesdl`
**요청**(GET 가능): `ses`, `clientid`, `backupid`, `path=<파일 경로>`
→ 파일 바이트를 직접 스트리밍(JSON 아님). `<a href>`/브라우저 다운로드로 처리.

### 5.5 폴더 ZIP 다운로드 — `sa=zipdl`
**요청**(GET 가능): `ses`, `clientid`, `backupid`, `path=<폴더 경로>`, (선택) `filter=<글롭>`
→ ZIP 스트림을 직접 반환.

### 5.6 클라이언트로 직접 복원(파일) — `sa=clientdl`
백업된 파일을 **원본 클라이언트(또는 다른 클라이언트)로 다시 내려보내 복원**한다.
**요청**: `ses`, `clientid`, `backupid`, `path`, (선택) `filter`
**응답**
```jsonc
{ "ok": "true", "wait_key": "..." }   // wait_key로 진행상황 폴링(restore_prepare_wait)
// 실패: {"err":"client_not_online"} | {"err":"internal_error"}
```

### 5.7 백업 보관/삭제 (`sa=backups` 부가 파라미터)
`sa=backups` 호출에 함께 보낸다.

| 파라미터 | 권한 | 효과 |
|----------|------|------|
| `archive=<backupid>` | manual_archive | 보관 설정 |
| `unarchive=<backupid>` | manual_archive | 보관 해제 |
| `delete=<backupid>` | delete_backups | 삭제 예약(이미지는 음수 id) |
| `stop_delete=<backupid>` | delete_backups | 삭제 예약 취소 |
| `delete_now=<backupid>` | delete_backups | 즉시 삭제(물리 확인 게이트 있을 수 있음) |

삭제 즉시 실행 시 에러: `{"delete_now_err": "delete_file_backup_failed" | "physical_confirmation_timeout"}`.

### 5.8 토큰 인증(엔드유저 파일 접근)
관리자 세션 없이 백업 토큰으로 자기 파일만 보는 모드. `backups` 요청에 `tokens0=...`,
`clientname=...`을 주면 서버가 토큰을 복호화해 익명 세션(`session`)을 발급하고 이후 그 세션으로
조회한다. 응답에 `token_authentication: true`가 붙는다. 일반 관리자 UI라면 5.1~5.7만 쓰면 된다.

---

## 6. 복원

### 6.1 파일 복원 → 클라이언트로 푸시
위 **5.6 `sa=clientdl`** 참조. 가장 일반적인 파일 복원 경로다.

### 6.2 이미지 복원 토큰 발급 — `restore_image`
이미지 복원은 클라이언트(복구 환경)가 서버에 접속해 수행하며, 웹은 **복원 토큰**을 발급한다.
**권한**: `browse_backups`(해당 clientid). **요청**: `ses`, `backupid=<이미지 backupid(양수 image id)>`

**응답** (`restore_image.cpp`)
```jsonc
{
  "ok": true,
  "token": "...",     // 복원 세션 토큰
  "authkey": "..."    // restore_authkey (복구 클라이언트가 사용)
}
```
이 token/authkey를 복구 환경(부팅 CD 등)에 전달하면 해당 이미지 복원이 인가된다.

### 6.3 복원 준비 대기 — `restore_prepare_wait`
`clientdl` 등에서 받은 `wait_key`로 복원 준비 완료를 폴링한다.
**요청**: `ses`, `wait_key=<...>` → 준비 상태/티켓 결과를 반환.

---

## 7. 로그

### 7.1 `logs` — 백업 로그 목록 / 단일 로그
**권한**: `logs`. **요청**: `ses`, (선택) `filter=<clientid,csv>`, `ll=<로그레벨>`, `logid=<단일 조회>`

**목록 응답** (`logs.cpp`, `logid` 없을 때)
```jsonc
{
  "clients": [ { "id": 1, "name": "DESKTOP-XYZ" } ],   // 로그 있는 클라이언트
  "log_right_clients": [ { "id": 1, "name": "..." } ], // 권한 있는 클라이언트
  "all_clients": true,        // (선택) 전체 권한
  "has_user": false,
  "filter": "1,2",
  "ll": 2,                    // 0=info,1=warning,2=error
  "logs": [
    {
      "id": 123,
      "name": "DESKTOP-XYZ",
      "time": 1719200000,     // epoch초
      "errors": 0,
      "warnings": 1,
      "image": 0,
      "incremental": 1,
      "resumed": 0,
      "restore": 0
    }
  ],
  "report_mail": "", "report_loglevel": "", "report_sendonly": ""
}
```

**단일 로그 응답** (`logid=<id>` 지정 시)
```jsonc
{
  "log": {
    "data": "<로그 본문 텍스트>",
    "time": 1719200000,
    "clientname": "DESKTOP-XYZ"
  }
}
```
> 로그 본문(`data`)은 줄마다 `레벨-시각-메시지` 형태로 인코딩돼 있다(레거시 UI가 파싱). 단순 표시는
> 그대로 출력해도 무방.

### 7.2 `livelog` — 실시간 로그 스트림
진행 중 작업의 라이브 로그. **요청**: `ses`, `clientid`, (선택) `logid`, `lastid`(이어받기).
**응답**: 증분 로그 엔트리 배열(반복 폴링하며 `lastid`를 갱신).

---

## 8. 통계 / 그래프

| 액션 | 권한 | 내용 |
|------|------|------|
| `usage` | `view_stats` 등 | 클라이언트별 사용 용량(파일/이미지/델타) 배열. `recalculate=true`로 재계산 트리거 |
| `usagegraph` | | 시간에 따른 사용량 그래프 데이터 |
| `piegraph` | `piegraph` | 클라이언트별 점유 파이차트 데이터 |

`usage` 응답 예: `{ "usage": [ { "name":"...", "files":N, "images":N, "used":N } ], "reset_statistics":"true"? }`

---

## 9. 사용자 / 설정 / 스크립트 (관리자)

| 액션 | 권한 | 용도 |
|------|------|------|
| `users` | `usermod` | 사용자 목록/추가/삭제/비밀번호 변경/권한 편집 |
| `settings` | `settings` | 전역/그룹/클라이언트별 설정 읽기·쓰기 |
| `add_client` | `add_client` | 새 클라이언트 등록(인증키/다운로드 정보 반환) |
| `download_client` | `add_client` | 클라이언트 설치 프로그램 다운로드(바이너리 스트림) |
| `scripts` | `manage_scripts` | 서버 사이드 스크립트 관리 |
| `shutdown` | admin | 서버 종료/재시작 등 |

이들 액션은 항목이 많아 본 문서 범위(상태 읽기 + 백업/복원) 밖이다. 필요 시 해당 `.cpp`의
`ACTION_IMPL`을 참조한다.

---

## 10. 권한 도메인 빠른 참조

`login` 응답과 `getRights()`에서 쓰이는 주요 도메인. 값은 `all`/`none`/`csv-clientid`.

| 도메인 | 의미 |
|--------|------|
| `status` | 상태 페이지 / 클라이언트 목록 |
| `progress` | 진행 상황 |
| `lastacts` | 최근 활동 |
| `start_backup` | 백업 시작 |
| `stop_backup` | 진행 작업 중단 |
| `browse_backups` | 백업 조회/파일 탐색/복원 |
| `delete_backups` | 백업 삭제 |
| `manual_archive` | 백업 보관 토글 |
| `logs` | 로그 열람 |
| `piegraph` / `graph` | 그래프 |
| `settings` | 설정 |
| `remove_client` | 클라이언트 제거 |
| `usermod` | 사용자 관리 |

---

## 11. 프론트엔드 구현 권장 흐름

1. 앱 시작 → `login`(ses 없이) 호출해 서버 기동 상태/익명 가능 여부 확인.
2. 로그인 폼 → `salt`(username) → 클라이언트 측 PBKDF2/MD5 해싱 → `login`. 세션 저장(localStorage).
3. 대시보드 → `status`(목록) + `progress`(폴링, 2~5초) + `lastacts`.
4. 백업 페이지 → `backups`(sa 없음→목록) → `sa=backups`(clientid) → `sa=files`(탐색).
5. 다운로드/복원 → `filesdl`/`zipdl`(직접 링크), `clientdl`(클라이언트로 복원), `restore_image`(이미지).
6. 로그 페이지 → `logs`(목록) → `logs?logid=`(상세) / `livelog`(진행 중).
7. 모든 응답에서 `error===1`이면 세션 만료 처리(재로그인).

> 참고 구현: 레거시 `urbackupserver/www/js/urbackup.js`(salt/login 해싱),
> `urbackupserver/www/js/urbackup_functions.js`(`getJSON` 호출/에러 규약).
