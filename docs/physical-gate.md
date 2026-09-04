# Physical Confirmation Gate (NeoBackup Ransom Defender)

파괴적(destructive) 백업 작업을 **물리 버튼** 승인 없이는 커밋하지 못하게 막는 게이트.
네트워크/관리자 계정이 장악돼도 원격으로는 백업 삭제가 불가능하다.

## 구조

버튼과 보안칩은 **`neobackup-gated` 헬퍼 데몬**이 소유한다. 서버는 유닉스 소켓으로
"승인해도 되는가"만 묻고, 칩 프로토콜이나 시리얼 장치를 전혀 알지 못한다.

```
urbackupsrv  --(AF_UNIX, JSON)-->  neobackup-gated  --(USB-CDC)-->  CH552T
 PhysicalGate                                                        +-- ALPU-C (I2C 0x3d)
                                                                     +-- 확인 버튼 (ch0)
```

프로세스를 나눈 이유:

- UrBackup 은 AGPLv3, 벤더 라이브러리는 바이너리 배포 → **라이선스 분리**
- 시리얼 포트 접근 권한이 서버로 새지 않음 → **권한 분리**
- 벤더 라이브러리의 아키텍처 제약이 서버 빌드에 전파되지 않음
- 데몬이 죽어도 서버는 살아 있고, 게이트만 fail-closed 된다

프로토콜 명세와 데몬 구현은 `neobackup-gated` 저장소의 `PROTOCOL.md` 참조.

## 변경된 파일

| 파일 | 변경 |
|------|------|
| `urbackupserver/PhysicalGate.h` / `.cpp` | 게이트 싱글톤. 헬퍼 데몬에 소켓으로 승인 요청 |
| `urbackupserver/serverinterface/backups.cpp` | `delete_now`(파일 백업, 양수 id) 삭제 직전 `requestApproval()` |
| `urbackupserver/dllmain.cpp` | 시작 시 `init()`, 종료 시 `destroy()` |
| `Makefile.am_server` | `PhysicalGate.cpp` 추가 |

서버는 libgpiod 를 링크하지 않는다.

## 동작

```
요청(delete_now) → 데몬에 await_approval (블로킹)
   ├─ 버튼 눌림 + 칩 인증 통과 → approved → 기존 ServerCleanupThread 삭제 실행
   ├─ 시간 내 버튼 없음         → timeout  → physical_confirmation_timeout
   └─ 칩 실패 / 장치 분리 / 데몬 불통 → denied → physical_confirmation_denied
```

**칩 인증은 버튼이 눌린 그 순간에 수행된다.** 부팅 시 1회만 검증하면 이후 장치를
가짜로 바꿔치기해도 알 수 없기 때문이다.

### fail-closed

확실하지 않으면 삭제하지 않는다. 데몬이 죽었거나, 소켓에 닿지 못하거나, 프로토콜
버전이 다르거나, 칩이 응답하지 않으면 모두 **거부**된다. 게이트를 우회해 삭제하려면
`physical_gate_disabled=true` 로 명시적으로 꺼야 한다.

## 현재 게이트 적용 범위

- 파일 백업 즉시 삭제(`delete_now`, 양수 backup id)
- 미적용(예정): 이미지 백업 삭제, `remove_client`, 보존 정책 단축
- 자동 롤링(retention)은 **의도적으로 게이트하지 않음** (스펙상 AUTO 경로)

> 보존 기간 단축은 삭제 API 가 아니라 설정 API 라 눈에 잘 띄지 않지만, 결과적으로
> 자동 롤링이 백업을 지운다. 게이트를 완성하려면 이 경로를 반드시 포함해야 한다.

## 설정

서버 파라미터 또는 `NEOBACKUP_*` 환경변수. 키 `foo` → `NEOBACKUP_FOO`.

| 키 | 기본값 | 설명 |
|----|--------|------|
| `physical_gate_disabled` | `false` | `true`면 게이트 비활성(삭제 즉시 허용) |
| `physical_gate_timeout_ms` | `90000` | 승인 대기(30000–300000으로 클램프) |
| `physical_gate_socket` | `/run/neobackup-gate.sock` | 헬퍼 데몬 소켓 |

감사 로그 경로는 데몬 쪽 설정(`-a`)이다. 기본값 `/var/log/urbackup_physical_gate.log`.

## 콘솔 상태 표시

1. **stderr 컬러 배너** — stderr 가 TTY 일 때 ANSI 컬러 박스를 출력.
   - 노랑: `PHYSICAL CONFIRMATION REQUIRED` (버튼 눌러라)
   - 초록: `APPROVED - COMMITTING`
   - 빨강: `TIMEOUT - REQUEST DISCARDED` / `DENIED - <reason>`
2. **일반 로그** — `/var/log/urbackup.log` 의 `PhysicalGate: ...`
3. **감사 로그** — 데몬이 기록. `pending` / `approved` / `timeout` / `denied_*`

```bash
tail -f /var/log/urbackup_physical_gate.log
journalctl -u neobackup-gated -f
```

배너 색까지 보려면 서버를 포그라운드로 실행한다(개발/데모용):

```bash
sudo systemctl stop urbackupsrv
sudo -u urbackup /usr/bin/urbackupsrv run --config /etc/default/urbackupsrv
```

## 권한

서버는 `urbackup` 유저로 동작하며, 소켓은 `root:urbackup 0660` 이다.
시리얼 장치 접근은 데몬(root)만 한다. 서버에 `dialout` 권한을 줄 필요가 없다.

`main.cpp` 는 권한 강등 시 `initgroups()` 로 보조 그룹을 유지하므로, `urbackup`
유저의 그룹 멤버십이 그대로 살아난다.

## 빌드

게이트를 위한 추가 의존성은 없다.

```bash
./switch_build.sh server
autoreconf --install
./configure --prefix=/usr --localstatedir=/var \
            --enable-embedded-cryptopp --enable-embedded-zstd \
            --enable-packaging --with-mountvhd CXXFLAGS="-g -O2"
make -j"$(nproc)"
```

헬퍼 데몬은 별도 저장소에서 빌드/설치한다.

## 검증

```bash
# 데몬 상태
echo '{"cmd":"status"}' | sudo -u urbackup nc -U /run/neobackup-gate.sock
# -> {"result":"ok","device":"connected","chip":"ok",...}

# 서버가 데몬을 인식했는지
grep PhysicalGate /var/log/urbackup.log
# -> PhysicalGate: helper daemon ready (device=connected, chip=ok).
```
