# NeoBackup Ransom Defender — 프론트엔드용 게이트 메모

> 동반 문서: 일반 API는 [web-api.md](web-api.md), 게이트 내부 동작/빌드/배선은 [physical-gate.md](physical-gate.md).
> 이 메모는 **UI가 어떤 엔드포인트에서 무엇을 다르게 처리해야 하는지**만 다룬다.

NeoBackup Ransom Defender는 스톡 UrBackup 위에 **물리 확인 게이트(Physical Confirmation Gate)**를
얹은 것이다. 파괴적 작업은 라즈베리파이의 **물리 버튼(GPIO)** 을 눌러야만 커밋된다.
네트워크/관리자 계정이 장악돼도 원격만으로는 백업을 삭제할 수 없다 — 누군가 기기 앞에서
버튼을 눌러야 한다. UI는 이 "사람이 직접 승인" 순간을 **우아하게** 보여주는 게 핵심이다.

근거 커밋: `1fb1cd69f5fdcd49d3ba5c691ac777761c72b0e5`.

---

## 1. 게이트가 붙은 엔드포인트는 단 하나

| 엔드포인트 | 트리거 | 게이트 |
|------------|--------|--------|
| `backups` (`sa=backups`) + `delete_now=<id>` | **파일 백업 즉시 삭제** (양수 backup id) | ✅ 물리 버튼 필요 |

그 외 액션(`status`, `progress`, `backups` 조회, `start_backup`, `restore_image` 등)에는
게이트가 **없다**. 일반 API 그대로 동작한다.

**아직 게이트 안 걸리는 파괴적 경로**(UI에서 "보호됨" 배지를 달면 안 됨):
- ❌ 이미지 백업 삭제 (`delete_now`에 **음수** id)
- ❌ 삭제 *예약*: `delete=<id>` / `stop_delete=<id>` (즉시 삭제가 아니라 예약만)
- ❌ `remove_client`(클라이언트 제거), 보존 정책/설정 변경
- ⚪ 자동 롤링(retention) 정리는 의도적으로 게이트하지 않음(AUTO 경로)

> 즉 UI에서 "물리 확인" 표시는 **파일 백업 + `delete_now` + 양수 id** 조합에서만 띄운다.

---

## 2. 요청/응답 — 무엇이 달라지나

호출 방식은 표준 `backups` 액션과 동일하다:

```
POST /x?a=backups
  ses=<session>
  sa=backups
  clientid=<id>
  delete_now=<backupid>      # 파일 백업이면 양수
```

차이점은 **응답이 늦게 온다는 것**과 **새 에러 코드**다.

### 2.1 동기 블로킹 (가장 중요한 UX 포인트)
`requestApproval()`은 **버튼이 눌릴 때까지 또는 타임아웃까지 HTTP 응답을 붙잡는다.**
즉 이 한 번의 요청이 **최대 ~90초(기본값, 30–300초 범위)** 동안 응답하지 않을 수 있다.

UI 측 필수 처리:
- 이 요청에는 **긴 타임아웃**을 설정(서버 타임아웃 + 여유, 예: `signal: AbortSignal.timeout(310_000)`).
- 일반 폴링/로딩 스피너가 아니라 **"기기에서 버튼을 누르세요" 대기 화면**으로 전환.
- 사용자가 취소하면 `AbortController`로 fetch만 끊는다(서버 측 PENDING은 타임아웃으로 자동 폐기됨).

### 2.2 응답 해석

| 응답 | 의미 | UI 처리 |
|------|------|---------|
| 응답 본문에 `delete_now_err` **없음** | 승인됨(버튼 눌림) → 삭제 실행됨 | ✅ "삭제 완료" — 초록 |
| `{"delete_now_err": "physical_confirmation_timeout"}` | 시간 내 버튼 안 눌림 → **삭제 안 됨** | ⏱️ "물리 확인 시간 초과, 백업은 그대로 보존됨" — 노랑/회색 |
| `{"delete_now_err": "delete_file_backup_failed"}` | 승인됐으나 실제 삭제 실패 | ❌ 오류 — 빨강 |
| `{"error": 1}` | 세션 만료 | 재로그인 |

> **게이트 비활성/미빌드 시**: libgpiod 없이 빌드했거나 `physical_gate_disabled=true`면 게이트는
> no-op이라 `requestApproval`이 즉시 통과한다. 응답이 바로 오고 `delete_now_err`도 없다.
> UI는 "대기 화면"을 띄웠더라도 응답이 즉시 오면 자연스럽게 완료로 넘어가도록 만들면 둘 다 커버된다.
> (게이트 활성 여부를 알려주는 별도 status 필드는 **아직 없다** — 4장 참고.)

---

## 3. 우아한 상태 표시 가이드 (브랜딩)

게이트의 생애주기는 서버 콘솔 배너 색과 동일하게 맞추면 브랜딩 일관성이 좋다
(physical-gate.md의 stderr 배너: 노랑→초록/빨강).

```
대기(awaiting)   ▶  PHYSICAL CONFIRMATION REQUIRED   — 노랑/앰버, 펄스 애니메이션
승인(approved)   ▶  PHYSICAL BUTTON PRESSED          — 초록
타임아웃(timeout)▶  TIMEOUT — REQUEST DISCARDED       — 회색/앰버 (실패 아님, 안전하게 보존)
실패(failed)     ▶  DELETE FAILED                    — 빨강
```

권장 권장 마이크로카피(한국어):
- 대기: **"기기의 보호 버튼을 눌러 삭제를 승인하세요."** + 남은 시간 카운트다운(서버 timeout 기준 추정)
- 부제: *"NeoBackup Ransom Defender — 원격만으로는 백업을 삭제할 수 없습니다."*
- 타임아웃: **"시간 내 확인이 없어 삭제를 취소했습니다. 백업은 안전합니다."**
- 승인: **"물리 확인됨. 백업을 삭제했습니다."**

UI 패턴 제안:
- 삭제 버튼 클릭 → 모달 오픈 → fetch 시작과 동시에 모달을 **"버튼 눌러주세요" 대기 상태**로.
- 카운트다운은 **표시용 추정치**일 뿐(서버가 실제 타임아웃 판정). 0이 돼도 곧장 실패로 단정하지 말고
  서버 응답을 기다린다.
- `delete=` / `stop_delete=`(예약) 경로에는 이 대기 UI를 쓰지 않는다 — 즉시 응답한다.

### 의사 코드
```ts
async function deleteNow(clientid: number, backupid: number) {
  // 파일 백업(양수)만 물리 게이트 대상
  const gated = backupid > 0;
  if (gated) showAwaitButtonModal();              // 앰버 펄스 + 카운트다운

  const data = await api("backups", {
    sa: "backups", clientid: String(clientid), delete_now: String(backupid),
  }, { timeoutMs: 310_000 });                     // 게이트 대기 여유

  if (data.error === 1) return toLogin();
  if (data.delete_now_err === "physical_confirmation_timeout")
    return showTimeout();                         // 회색/앰버 "안전하게 보존됨"
  if (data.delete_now_err === "delete_file_backup_failed")
    return showError();                           // 빨강
  showApprovedAndRefresh();                        // 초록 → 목록 갱신
}
```

---

## 4. 알아둘 한계 / 향후 훅 포인트

- **게이트 상태 API 없음**: 현재 어떤 액션도 "게이트 활성/대기 중/마지막 승인 시각"을 JSON으로
  노출하지 않는다. UI는 `delete_now` 응답으로만 게이트 존재를 추론한다. 라이브 대시보드 배지
  ("🛡️ Ransom Defender 활성")가 필요하면 `status` 응답이나 신규 액션에 필드 추가가 선행돼야 한다.
- **감사 로그는 서버 파일**: 승인/타임아웃 이력은 `/var/log/urbackup_physical_gate.log`(append-only).
  웹에서 보여주려면 이를 노출하는 엔드포인트가 별도로 필요(현재 없음).
- **동시 PENDING**: 여러 삭제가 동시에 대기하면 **버튼 한 번에 하나가 승인**된다(press_count 증가분
  하나당 대기 요청 하나 해소). UI에서 동시에 여러 삭제를 띄우는 건 지양하고, 한 번에 하나씩 권장.
- **음수 id(이미지)는 게이트 미적용**: 같은 `delete_now`라도 이미지 삭제는 즉시 실행된다. UI에서
  "보호됨" 표식을 파일/이미지에 무분별하게 달지 말 것.

---

## 5. 한 줄 요약

> **파일 백업을 `delete_now`(양수 id)로 즉시 삭제할 때만** 요청이 ~90초까지 블로킹되며,
> 물리 버튼이 눌리면 삭제, 안 눌리면 `physical_confirmation_timeout`으로 안전하게 취소된다.
> UI는 이 구간을 "기기 버튼을 눌러 승인" 대기 화면으로 우아하게 표현하면 된다.
