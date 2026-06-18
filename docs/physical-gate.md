# Physical Confirmation Gate (NeoBackup Ransom Defender)

파괴적(destructive) 백업 작업을 **GPIO 17 물리 버튼** 승인 없이는 커밋하지 못하게 막는 게이트.
네트워크/관리자 계정이 장악돼도 원격으로는 백업 삭제가 불가능하다.

## 변경 요약

| 파일 | 변경 |
|------|------|
| `urbackupserver/PhysicalGate.h` / `.cpp` | **신규.** 게이트 싱글톤 + GPIO 17 watcher 스레드 + append-only 감사 로그 |
| `urbackupserver/serverinterface/backups.cpp` | `delete_now`(파일 백업, 양수 id) 실제 삭제 직전에 `PhysicalGate::requestApproval()` 통과. 타임아웃 시 `physical_confirmation_timeout` 반환하고 삭제 안 함 |
| `urbackupserver/dllmain.cpp` | 시작 시 `PhysicalGate::init()`, 종료 시 `PhysicalGate::destroy()` |
| `Makefile.am_server` | `PhysicalGate.cpp` 추가, `WITH_LIBGPIOD`일 때 `-lgpiod` + `-DWITH_LIBGPIOD` |
| `configure.ac_server` | `--disable-libgpiod` 옵션 + libgpiod 자동 감지(`AM_CONDITIONAL(WITH_LIBGPIOD)`) |

## 동작

```
요청(delete_now) → PENDING 큐 등록(감사 로그) → GPIO 17 눌림 대기(기본 90초)
   ├─ 버튼 눌림  → APPROVED  → 기존 ServerCleanupThread 삭제 실행
   └─ 타임아웃   → TIMEOUT   → 삭제 폐기, UI에 physical_confirmation_timeout
```

- libgpiod 없이 빌드되면 게이트는 **no-op**(`Disabled`)으로 동작 → 이식성/ARM 안전.
- 감지 방식: **falling edge**(눌리는 순간), 내부 **pull-up bias**, 250ms 디바운스.
- 따라서 **모멘터리(순간) 스위치**를 전제로 한다. (래칭 스위치는 §아래 참고)

## 현재 게이트 적용 범위

- ✅ 파일 백업 즉시 삭제(`delete_now`, 양수 backup id)
- ❌ 미적용(예정): 이미지 백업 삭제, `remove_client`, 보존 정책 단축/설정 변경
- ⚪ 자동 롤링(retention)은 **의도적으로 게이트하지 않음** (스펙상 AUTO 경로)

## 설정 (선택)

서버 파라미터 또는 `NEOBACKUP_*` 환경변수로 조정. 키와 env 매핑은 `key` → `NEOBACKUP_KEY`.

| 키 | 기본값 | 설명 |
|----|--------|------|
| `physical_gate_disabled` | `false` | `true`면 게이트 비활성(삭제 즉시 허용) |
| `physical_gate_timeout_ms` | `90000` | 승인 대기(30000–300000으로 클램프) |
| `physical_gate_gpio_chip` | `gpiochip0` | GPIO 칩 이름 |
| `physical_gate_gpio_line` | `17` | GPIO 라인 오프셋(BCM 번호) |
| `physical_gate_audit_log` | `/var/log/urbackup_physical_gate.log` | append-only 감사 로그 |

## 콘솔 상태 표시 (간지나는 배너)

게이트는 두 가지로 표시된다:

1. **stderr 컬러 배너** — stderr가 TTY(파이 콘솔)일 때 ANSI 컬러로 큼직한 박스를 직접 출력.
   - 노랑: `PHYSICAL CONFIRMATION REQUIRED` (삭제 대기, 버튼 눌러라)
   - 초록: `PHYSICAL BUTTON PRESSED` / `APPROVED - COMMITTING`
   - 빨강: `TIMEOUT - REQUEST DISCARDED`
   - 청록: 대기 중인 작업이 없을 때 버튼만 눌린 경우
2. **일반 로그** — `/var/log/urbackup.log`에 `PhysicalGate: ...` 라인.

### 파이 콘솔에 띄워두기

systemd 데몬은 stderr가 콘솔이 아니라 journal로 가므로 배너 색은 빠지지만 박스는 보인다.
실시간으로 보려면 콘솔에서 아래 중 하나:

```bash
# A) 게이트 전용 로그만 (가장 깔끔)
tail -f /var/log/urbackup_physical_gate.log

# B) 서버 전체 로그에서 게이트 라인만 강조
tail -F /var/log/urbackup.log | grep --line-buffered --color=always PhysicalGate

# C) systemd journal 실시간
journalctl -u urbackupsrv -f
```

배너 색까지 보고 싶으면 데몬을 **포그라운드로 직접 실행**해 stderr를 콘솔에 붙인다(개발/데모용):

```bash
sudo systemctl stop urbackupsrv
sudo -u urbackup /usr/bin/urbackupsrv run --config /etc/default/urbackupsrv
# 이 터미널에 컬러 배너가 그대로 뜬다. Ctrl-C로 종료 후 systemctl restart 로 복귀.
```

## 권한 주의

서버는 `urbackup` 유저로 동작하므로 GPIO 캐릭터 디바이스 접근 권한이 필요하다.
`gpio` 그룹에 추가하거나 udev 규칙으로 `/dev/gpiochip0` 권한을 부여할 것.

```bash
sudo usermod -aG gpio urbackup   # 또는 udev 규칙
```

## 빌드

```bash
sudo apt-get install -y libgpiod-dev
./switch_build.sh server
autoreconf --install
./configure --prefix=/usr --localstatedir=/var \
            --enable-embedded-cryptopp --enable-embedded-zstd \
            --enable-packaging --with-mountvhd CXXFLAGS="-g -O2"
make -j"$(nproc)"
# 검증
ldd urbackupsrv | grep gpiod      # libgpiod.so.2 링크 확인
```
