# NeoBackup 프론트엔드 — 실제 API 마이그레이션 & 테스트 가이드

이 스킨(`NeoBackup.dc.html`)은 현재 **데모 모드**로 동작합니다. 화면·플로우는 전부 실제 UI지만,
데이터는 컴포넌트 안의 목업으로 채워지고 백업 진행은 타이머로 시뮬레이션됩니다.
이 문서는 그 목업을 **실제 UrBackup 서버 Web API**로 교체하는 방법을 단계별로 설명합니다.

> 근거 문서: `web-api.md`(일반 API), `web-api-neobackup-gate.md`(게이트), `physical-gate.md`(게이트 내부).

---

## 0. 큰 그림

| 구성 | 현재(데모) | 실제 연동 후 |
|------|-----------|-------------|
| 데이터 | 생성자(`constructor`)의 목업 배열 | `this.api(action, params)` 호출 결과 |
| 진행률 | `simulate()` / `streamLive()` 타이머 | `progress` / `livelog` 폴링 |
| 세션 | 없음(`login`이 `authed=true`만 세팅) | `salt`→`login` 챌린지·응답, `this._ses` 저장 |
| 게이트 | `버튼 누르기` 데모 클릭 | 서버가 ~90초 블로킹하는 `delete_now` 응답 |

핵심은 이미 들어 있는 **`api()` 래퍼**(`NeoBackup.dc.html`의 로직 클래스)와,
각 화면이 읽는 **`renderVals()`의 데이터 소스**만 바꾸면 된다는 점입니다.

---

## 1. 서버에 올리고 같은 출처로 서빙하기

API는 `POST /x?a=<action>` (기본 포트 **55414**) 규약입니다. 가장 단순한 배포는
**빌드 결과물을 UrBackup 웹 루트에 같이 서빙**해 동일 출처(same-origin)로 만드는 것입니다.

```bash
# 1) DC를 단일 HTML로 번들 (이 프로젝트에서 'Save as standalone HTML' 사용)
#    → NeoBackup.standalone.html 생성
# 2) UrBackup 서버의 www 디렉터리에 배치
sudo cp NeoBackup.standalone.html /usr/share/urbackup/www/neo.html
# 3) 접속: http://<server>:55414/neo.html
```

`api()`는 `fetch('/x?a=...')`처럼 **상대 경로**를 씁니다. 같은 출처면 CORS·포트 문제가 없습니다.

> **다른 출처에서 띄워야 한다면**(예: 개발 서버 `localhost:5173` → 서버 `:55414`):
> `api()`의 URL을 절대 경로로 바꾸고(`const BASE='http://server:55414'`), 서버에 CORS 허용이
> 필요합니다. UrBackup은 기본적으로 교차 출처 쿠키/세션을 전제하지 않으므로 **같은 출처 배포를 강력 권장**합니다.

---

## 2. 데모 스위치 추가 (점진적 전환용)

로직 클래스 맨 위에 플래그 하나를 둬서 화면별로 하나씩 실 API로 옮기면 안전합니다.

```js
// constructor 안, this.state 위
this.DEMO = false;   // true면 지금처럼 목업, false면 실제 서버
```

이후 각 로더에서 `if (this.DEMO) { ...목업... } else { ...api... }`로 분기합니다.

---

## 3. 인증 — `salt` → 해싱 → `login`

현재 로그인 버튼은 `this.setState({authed:true})`만 합니다. 실제 구현은 2단계입니다.

### 3.1 의존성
클라이언트 측 해싱이 필요합니다. `<helmet>`에 crypto-js를 추가하세요.

```html
<!-- NeoBackup.dc.html 의 <helmet> 안 -->
<script src="https://cdnjs.cloudflare.com/ajax/libs/crypto-js/4.2.0/crypto-js.min.js"></script>
```

### 3.2 로그인 핸들러 교체
`renderVals()`의 `login:()=>this.setState({authed:true})`를 아래 메서드 호출로 바꿉니다.

```js
async doLogin(username, password){
  // 1) salt + 세션 + 챌린지 요청
  const s = await this.api('salt', { username });
  if (s.error === 3) return this.setState({ loginErr: '잠시 후 다시 시도하세요 (rate limit)' });
  this._ses = s.ses;                       // 이후 모든 요청에 첨부
  // 2) 비밀번호 해시 (레거시 www/js/urbackup.js 와 동일)
  const CJS = window.CryptoJS;
  let pwmd5;
  if (s.pbkdf2_rounds > 0) {
    pwmd5 = CJS.PBKDF2(password, CJS.enc.Hex.parse(s.salt),
              { keySize: 256/32, iterations: s.pbkdf2_rounds, hasher: CJS.algo.SHA256 }).toString();
  } else {
    pwmd5 = CJS.MD5(s.salt + password).toString();
  }
  const password_field = CJS.MD5(s.rnd + pwmd5).toString();
  // 3) 로그인
  const r = await this.api('login', { username, password: password_field });
  if (r.success) {
    this._ses = r.session || this._ses;
    this._rights = r;                      // status/browse_backups/... 권한 게이팅에 사용
    this.setState({ authed: true });
    this.loadAll();                        // 대시보드 초기 로드
  } else if (r.error === 2) {
    this.setState({ loginErr: '사용자명 또는 비밀번호가 틀립니다' });
  }
}
```

`api()`는 이미 `this._ses`를 자동 첨부합니다(코드 참고). 세션을 새로고침 후에도 유지하려면
`localStorage.setItem('nb_ses', this._ses)`로 저장하고 `componentDidMount`에서 복구하세요.

### 3.3 권한 게이팅
`login` 응답의 도메인 값(`"all" | "none" | "1,2,3"`)으로 사이드바·버튼을 가립니다.

```js
hasRight(domain){ const v=this._rights?.[domain]; return v && v!=='none'; }
// 예: nav 정의에서 status 권한 없으면 대시보드 항목 제거
```

---

## 4. 화면별 데이터 소스 교체

아래 표의 **목업 위치**를 `api()` 호출로 바꾸면 됩니다. 응답 필드명은 목업과 거의 일치하도록 맞춰뒀습니다.

| 화면 | 액션 | 교체 대상(현재 목업) | 비고 |
|------|------|----------------------|------|
| 대시보드 | `status` | `this.state.clients` | `status` 코드·`processes`로 진행 표시 |
| 진행 상황 | `progress` | `this.state.progress` | 2~5초 폴링. `with_lastacts=0` 가능 |
| 라이브로그 | `livelog` | `streamLive()` | `clientid`+`lastid`로 이어받기 |
| 최근 활동 | `lastacts` | `this.state.lastacts` | |
| 백업 목록 | `backups`(sa 없음→`sa=backups`) | `this.state.bk` | clientid별 |
| 파일 탐색 | `backups&sa=files` | `this.tree` | `path`, 이미지면 음수 backupid |
| 다운로드 | `filesdl`/`zipdl` | `onZip` 등 | `<a href>` 직접 링크 |
| 파일 복원 | `clientdl` | `restoreFolderToClient()` | `wait_key`로 폴링 |
| 이미지 토큰 | `restore_image` | `issueImageToken()` | `token`/`authkey` 반환 |
| 로그 | `logs` / `logs?logid=` | `this.state.logs`/`logBodies` | |
| 통계 | `usage` / `piegraph` | `this.state.usage` | |

### 4.1 예시 — 대시보드(status) 폴링

```js
async loadStatus(){
  if (this.DEMO) return;                       // 데모면 목업 유지
  const d = await this.api('status');
  if (d.error === 1) return this.toLogin();    // 세션 만료
  // 응답 → 컴포넌트 형태로 매핑 (필드명 거의 동일)
  const clients = d.status.map(c => ({
    id:c.id, name:c.name, os:c.os_simple, osv:c.os_version_string, cv:c.client_version_string,
    ip:c.ip, online:c.online, lastbackup:c.lastbackup, lastimage:c.lastbackup_image,
    issues:c.last_filebackup_issues, status:c.status, nopath:!!c.no_backup_paths,
  }));
  this.setState({ clients });
}
componentDidMount(){
  // 데모 타이머 대신:
  if (!this.DEMO) this._poll = setInterval(()=>{ this.loadStatus(); this.loadProgress(); }, 3000);
}
```

### 4.2 다운로드는 직접 링크
`filesdl`/`zipdl`은 JSON이 아니라 바이트 스트림입니다. fetch가 아니라 링크로 처리하세요.

```js
fileHref(clientid, backupid, path){
  const q = new URLSearchParams({ ses:this._ses, clientid, backupid, path });
  return `/x?a=backups&sa=filesdl&${q}`;        // 폴더 ZIP은 sa=zipdl
}
// 템플릿: 다운로드 버튼을 <a href="{{ f.href }}" download> 로
```

> **이미지 백업은 음수 backupid**입니다(이미지 id 7 → `backupid=-7`). 파일 탐색·복원 시 부호 주의.

---

## 5. 물리 게이트 — 가장 중요한 UX 포인트

`delete_now`(**파일 백업, 양수 id**)는 서버가 **버튼이 눌릴 때까지 또는 타임아웃까지 응답을 붙잡습니다**
(기본 ~90초, 30–300초). UI는 이미 "버튼 누르세요" 대기 화면이 준비돼 있으니, **데모 클릭을 실제 요청으로** 바꾸기만 하면 됩니다.

```js
async openGate(cid, backup, kind){
  const deleteId = kind === 'image' ? -backup.id : backup.id;
  const gated = deleteId > 0 && this.state.gateEnabled;   // 파일+양수만 게이트
  if (!gated) { /* 이미지/비활성: 즉시 확인 모달 (기존 로직 유지) */ return; }

  // 대기 화면 띄우고 카운트다운 시작 (표시용 추정치)
  const total = this.state.gateTimeout;
  this.setState({ gate:{ open:true, phase:'awaiting', cid, bid:backup.id, kind, total, remaining:total }});
  this.startCountdown();

  this._abort = new AbortController();
  try {
    // ★ 긴 타임아웃 + abort 가능. 데모의 gatePress() 대신 이 요청이 실제로 블로킹됩니다.
    const data = await this.api('backups',
      { sa:'backups', clientid:String(cid), delete_now:String(deleteId) },
      310000 /* 서버 타임아웃 + 여유 */);
    this.stopCountdown();
    if (data.error === 1) return this.toLogin();
    if (data.delete_now_err === 'physical_confirmation_timeout')
      return this.setState(s=>({ gate:{...s.gate, phase:'timeout' }}));
    if (data.delete_now_err === 'delete_file_backup_failed')
      return this.setState(s=>({ gate:{...s.gate, phase:'failed' }}));
    // 에러 키 없음 = 승인됨 → 삭제 완료
    this.setState(s=>({ gate:{...s.gate, phase:'approved' }}));
    setTimeout(()=>this.commitDelete(), 1300);   // 목록에서 제거 후 'deleted'
  } catch(e){
    // 사용자가 취소(abort)했거나 네트워크 오류
    this.stopCountdown();
  }
}
```

체크리스트:
- 이 요청에만 **긴 타임아웃**(예: `AbortSignal.timeout(310_000)`). `api()` 4번째 인자로 전달.
- 카운트다운은 **표시용 추정치**. 0이 돼도 곧장 실패로 단정하지 말고 서버 응답을 기다립니다.
- 취소 버튼은 `this._abort.abort()`만 호출 — 서버 PENDING은 타임아웃으로 자동 폐기됩니다.
- **게이트 비활성/미빌드 시**(libgpiod 없음 또는 `physical_gate_disabled=true`): 응답이 즉시 옵니다.
  대기 화면을 띄웠더라도 응답이 바로 오면 자연스럽게 완료로 넘어가므로 코드 변경 불필요.
- 동시에 여러 삭제 대기는 지양(버튼 한 번에 하나 승인). **한 번에 하나씩** 권장.
- **이미지 삭제/`remove_client`/예약 삭제(`delete`/`stop_delete`)에는 게이트가 없습니다** — "보호됨" 배지를 달지 마세요.

물리 버튼 실측 테스트:
```bash
# 서버를 포그라운드로 띄워 컬러 배너 확인 (개발/데모용)
sudo systemctl stop urbackupsrv
sudo -u urbackup /usr/bin/urbackupsrv run --config /etc/default/urbackupsrv
# UI에서 '즉시 삭제' → 콘솔에 노랑 'PHYSICAL CONFIRMATION REQUIRED' → GPIO26 버튼 → 초록 'APPROVED'
tail -f /var/log/urbackup_physical_gate.log   # 감사 로그(append-only)
```

---

## 6. 공통 에러 처리

모든 응답에서 먼저 세션 만료를 검사하세요(이미 패턴 제공).

```js
// api() 호출부 공통
if (data.error === 1) return this.toLogin();   // 세션 없음/만료 → 재로그인
// error 2: 인증 실패/대상 없음 → 메시지, error 3: rate limit → 재시도 안내
// {err:'access_denied'} : backups 토큰/권한 거부
toLogin(){ this._ses=null; localStorage.removeItem('nb_ses'); this.setState({ authed:false }); }
```

---

## 7. 테스트 플로우 (실서버)

1. **빌드·배포** → `http://<server>:55414/neo.html` 접속(같은 출처).
2. **로그인** — `salt`→해싱→`login` 성공, 사이드바가 권한대로 표시되는지.
3. **대시보드** — 클라이언트 목록·온라인·상태 코드·진행률이 실제와 일치.
4. **백업 시작** — `start_backup`(`incr_file`) 후 진행 상황 탭에서 진행 바·라이브로그 갱신.
5. **백업 조회 → 파일 탐색** — `sa=backups`→`sa=files`, 다운로드/ZIP 링크 동작.
6. **물리 게이트** — 파일 백업 `즉시 삭제` → 대기 화면 → **GPIO 버튼** → 삭제 완료. 버튼 안 누르고 타임아웃 → "안전하게 보존됨".
7. **이미지 복원** — `restore_image` 토큰·authkey 발급 확인.
8. **로그/통계** — `logs`/`logs?logid=`, `usage`/`piegraph` 표시.
9. **세션 만료** — 세션 무효화 후 임의 액션 → 자동 재로그인 전환.

---

## 8. 알아둘 한계 (문서 §4 기준)

- **게이트 상태 API 없음**: 어떤 액션도 "게이트 활성/대기 중/마지막 승인 시각"을 JSON으로 노출하지 않습니다.
  탑바의 "Ransom Defender 활성" 배지는 현재 **정적 표시**입니다. 라이브 배지를 원하면 `status` 응답에 필드 추가가 선행돼야 합니다.
- **감사 로그는 서버 파일**(`/var/log/urbackup_physical_gate.log`). 웹 노출용 엔드포인트는 아직 없습니다.
- 설정·사용자·스크립트(`settings`/`users`/`scripts` 등)는 항목이 많아 본 스킨에서는 **읽기·표시 위주**로 구성했습니다.

---

## 부록 — 파일 구성

```
NeoBackup.dc.html   # 프론트엔드(템플릿 + 로직). 브라우저에서 바로 열림
support.js          # DC 런타임(자동 생성, 수정하지 말 것)
MIGRATION.md        # 이 문서
uploads/            # 원본 API 레퍼런스(web-api.md 등)
```
