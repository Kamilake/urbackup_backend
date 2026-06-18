# AGENTS.md — UrBackup Server (소스 빌드 / 운영 메모)

이 저장소는 **UrBackup 서버**를 소스에서 빌드해 라즈베리파이(ARM64, Ubuntu 24.04)에
배포·운영하기 위한 작업 트리입니다. 아래 내용은 모두 **실제 이 환경에서 검증된 사실**이며,
과거에 실제로 발생했던 실수와 그 해결책을 기록해 둔 것입니다. 코드를 수정하거나 다시
빌드/설치하기 전에 반드시 훑어보세요.

> 검증 환경: Raspberry Pi, `aarch64`, Ubuntu 24.04.4 LTS, 호스트네임 `ransome-defender-pi`

---

## 1. 저장소 / 브랜치

- 작업 트리: `~/urbackup_backend`
- 리모트:
  - `origin`  → `https://github.com/Kamilake/urbackup_backend.git` (본인 포크)
  - `upstream` → `https://github.com/uroni/urbackup_backend.git` (공식)
- **운영 브랜치: `2.5.x`** ← 반드시 이 브랜치에서 빌드할 것.

### ⚠️ `dev` 브랜치를 쓰지 말 것 (DB 버전 호환성)

| 브랜치/태그 | 지원 DB 스키마 상한 (`max_v`) | 비고 |
|------------|------------------------------|------|
| `2.5.x`    | **69** ✅                    | 운영용. 현재 DB(=69)와 일치 |
| `2.5.37` 태그 | 69 ✅                      | 고정 릴리스 |
| `dev`      | 66 ❌                        | DB 69를 못 열고 `exit(4)`로 거부 |
| `next`     | 47 ❌                        | 훨씬 오래됨 |

- 상한 값 위치: `urbackupserver/dllmain.cpp`의 `int max_v=69;`
- 거부 로직: 같은 파일 `if(ver>max_v) { ... "only supports databases up to version ..." ; exit(4); }`
- 현재 운영 DB 스키마 버전은 **69**. 빌드하려는 브랜치의 `max_v`가 69 미만이면
  서버가 기존 DB를 열지 못하고 즉시 종료된다. 빌드 전 항상 확인:
  ```bash
  grep -m1 "int max_v=" urbackupserver/dllmain.cpp
  sudo sqlite3 /var/urbackup/backup_server.db "SELECT tvalue FROM misc WHERE tkey='db_version';"
  ```

---

## 2. 데이터 위치 (가장 자주 헷갈리는 부분)

UrBackup은 **두 개의 서로 다른 경로**를 쓴다. 절대 혼동하지 말 것:

| 경로 | 역할 | 핵심 내용 |
|------|------|-----------|
| **`/var/urbackup`** | **작업 디렉토리(CWD) = 활성 데이터** | `backup_server*.db`(활성 DB), `server_ident*`(서버 키), `backupfolder`, `fileindex/` |
| **`/srv/urbackup`** | **백업 저장 폴더** | 클라이언트별 백업본(`exjang/`, `DESKTOP-KFMQTSH/`). 안에 DB/키 *사본*도 있음 |

- 진짜 활성 DB·키는 **`/var/urbackup`** 에 있다. `/srv/urbackup/urbackup/`의 DB·키는
  저장 폴더 안의 보조 사본이므로, **백업/복구 시 `/var/urbackup`을 기준으로 삼을 것.**
- 백업 저장 위치는 `/var/urbackup/backupfolder` 파일 내용으로 결정됨 (현재 `/srv/urbackup`).
- 설정: `/etc/default/urbackupsrv` (포트, 유저, 로그 등), `/etc/urbackup/backupfolder`.

### 서버는 키 파일을 상대경로로 연다 → CWD가 핵심
서버는 `urbackup/server_ident_ecdsa409k1.priv` 같은 파일을 **CWD 기준 상대경로**로 연다
(`urbackupserver/dllmain.cpp`). 따라서 CWD가 반드시 `/var`(= `VARDIR`)여야 하며,
그래야 `/var/urbackup/...`를 찾는다. CWD가 틀리면:
```
ERROR: ... FileSink: error opening file for writing: urbackup/server_ident_ecdsa409k1.priv
```

---

## 3. ⚠️ 빌드 시 가장 중요한 함정: `--localstatedir=/var`

`run` 서브커맨드는 시작 시 자동으로 `--workingdir VARDIR`을 주입한다
(`urbackupserver/cmdline_preprocessor.cpp`, "`real_args.push_back("--workingdir"); real_args.push_back(VARDIR);`").
그리고 `main.cpp`가 그 값으로 `chdir()` 한다 (`main.cpp`의 `chdir((Server->getServerWorkingDir())...)`).

경로 매크로는 컴파일 타임에 Makefile에서 주입된다:
- `-DVARDIR='"$(localstatedir)"'`
- `-DDATADIR='"$(datadir)"'`  → `--http_root $(datadir)/urbackup/www` 등에 사용
- `-DBINDIR='"$(bindir)"'`

문제: `--prefix=/usr`만 주면 `localstatedir = ${prefix}/var = /usr/var` 가 되어
**CWD가 `/usr/var`로 잡히고 서버가 키를 못 찾아 죽는다.**

✅ 해결: configure에 반드시 **`--localstatedir=/var`** 를 추가한다.
검증된 경로 해석 결과:
```
prefix = /usr
datadir = /usr/share        (→ www: /usr/share/urbackup/www)
localstatedir = /var        (→ CWD/데이터: /var/urbackup)
bindir = /usr/bin
```

### 매크로 변경이 반영 안 될 때 (증분 빌드 함정)
`VARDIR`/`DATADIR`는 컴파일 타임 매크로라, configure만 다시 해도 이미 만들어진 `.o`는
재컴파일되지 않는다. 경로를 바꿨으면 관련 오브젝트를 강제로 지우고 다시 make:
```bash
rm -f urbackupserver/urbackupsrv-cmdline_preprocessor.o \
      urbackupclient/urbackupsrv-cmdline_preprocessor.o \
      urbackupclient/urbackupsrv-ClientService.o
make -j"$(nproc)"
# 검증: 바이너리에 /var 가 박혔는지
strings urbackupsrv | grep -E "^/var$"
```

---

## 4. 전체 빌드 절차 (검증됨)

```bash
cd ~/urbackup_backend
git checkout 2.5.x                 # 반드시 2.5.x

./switch_build.sh server           # Makefile.am_server → Makefile.am 등으로 교체, BUILDID 증가
./download_cryptopp.sh             # embedded crypto++ 소스 받기 (embedded 옵션 쓸 때 필수)
autoreconf --install               # configure 생성 (m4 경고는 무시 가능)

./configure --prefix=/usr --localstatedir=/var \
            --enable-embedded-cryptopp --enable-embedded-zstd \
            --enable-packaging --with-mountvhd \
            CXXFLAGS="-g -O2"

make -j"$(nproc)"                  # 라즈베리파이는 오래 걸림. crypto++ 구간이 가장 무겁다
```

빌드 산출물: `./urbackupsrv` (디버그 심볼 포함이라 ~128MB).

### 빌드 관련 주의
- **전원 안정 필수.** 빌드 중 전원이 끊기면 `.o`/`.lo`가 0바이트로 손상되어
  다음 빌드가 `'... is not a valid libtool object` 류로 실패한다. 복구:
  ```bash
  find . \( -name '*.o' -o -name '*.lo' \) -size 0 -delete   # 손상 파일만 제거
  make -j"$(nproc)"                                          # 증분 재빌드
  ```
- 의존성(검증됨): `build-essential autoconf automake libtool libssl-dev libcurl4-openssl-dev liblmdb-dev`
  (embedded crypto++/zstd를 쓰므로 그쪽 시스템 라이브러리는 불필요.)
- `switch_build.sh`는 모드별 파일(`Makefile.am_server`, `configure.ac_server`,
  `defaults_server`, `init.d_server`)을 활성 파일로 복사한다. **`Makefile.am`/`configure.ac`는
  생성물이므로 직접 편집하지 말고, `*_server` 원본을 수정**할 것.

---

## 5. ARM(aarch64) 빌드 참고

- **`2.5.x`는 ARM 빌드 문제 없음.** (x86 전용 AWS SDK 파일을 빌드 대상에 넣지 않음)
- `dev` 브랜치에는 ARM 빌드 버그가 있다: `external/aws-cpp-sdk/AwsCCommon/source/arch/cpuid.c`,
  `encoding_avx2.c`가 `immintrin.h`(x86 전용)를 무조건 include → aarch64에서
  `fatal error: immintrin.h: No such file or directory`. dev 기반으로 upstream에 기여한다면
  이 파일들을 `#if defined(__i386__) || defined(__x86_64__)` 가드로 감싸는 것이 올바른 수정
  (비-x86에서는 `aws_common_private_has_avx2()`가 `false`를 반환하는 스텁만 남기면 됨).
- configure는 ARMv8 crypto 가속 플래그(`-march=armv8-a+crypto` 등)를 자동 감지한다.

---

## 6. 설치 / 교체 절차 (검증됨)

기존 패키지(`PREFIX=/usr`)와 같은 경로를 쓰므로, **데이터/서비스 설정은 건드리지 않고
바이너리 + `/usr/share/urbackup`만 교체**한다.

```bash
# 1) 서비스 정지 (RemainAfterExit=yes 이므로 start는 no-op일 수 있음 → stop 먼저)
sudo systemctl stop urbackupsrv

# 2) (한 번만) 롤백용 백업
sudo cp -a /usr/bin/urbackupsrv      /usr/bin/urbackupsrv.pkg-2.5.38.bak
sudo cp -a /usr/share/urbackup       /usr/share/urbackup.pkg-2.5.38.bak

# 3) strip 후 바이너리 설치 (디버그 심볼 제거: ~128MB → ~9MB)
strip -o /tmp/urbackupsrv.stripped urbackupsrv
sudo install -m755 /tmp/urbackupsrv.stripped /usr/bin/urbackupsrv

# 4) www UI 등 share 자산 갱신 (DESTDIR staging 권장)
make install DESTDIR=/tmp/urbk-stage
sudo cp -a /tmp/urbk-stage/usr/share/urbackup/. /usr/share/urbackup/

# 5) ⚠️ 권한 정정 (아래 참조)
sudo chown -R root:root /usr/share/urbackup
sudo find /usr/share/urbackup -type d -exec chmod 755 {} \;
sudo find /usr/share/urbackup -type f -exec chmod 644 {} \;

# 6) 재기동
sudo systemctl restart urbackupsrv
```

### ⚠️ 웹 UI 404 → 권한 문제
`cp -a`로 staging을 복사하면 빌드 유저 소유권/권한이 따라와
`/usr/share/urbackup`이 `urbackup` 서비스 유저로 읽히지 않아 **웹 UI가 모든 경로에서 404**가 된다.
반드시 `root:root` + 디렉토리 755 / 파일 644로 정정할 것 (위 5단계).

---

## 7. 서비스 / 검증

- 서비스: `urbackupsrv` (systemd가 `/etc/init.d/urbackupsrv`를 sysv-generator로 감싼 형태).
  유닛 타입 `forking` + `RemainAfterExit=yes` → 상태가 `active (exited)`로 남을 수 있다.
  데몬을 실제로 다시 띄우려면 `start`가 아니라 **`restart`**(또는 `stop`→`start`).
- 실행 커맨드: `/usr/bin/urbackupsrv run --config /etc/default/urbackupsrv --daemon --pidfile /var/run/urbackupsrv.pid`
- 포트: FastCGI **55413**, 웹 UI(HTTP) **55414**.
- 로그: `/var/log/urbackup.log`.

빠른 헬스체크:
```bash
pgrep -fa "urbackupsrv run"                                   # 프로세스 생존
sudo readlink /proc/$(pgrep -f 'urbackupsrv run'|head -1)/cwd # 반드시 /var
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:55414/   # 200 기대
sudo sqlite3 /var/urbackup/backup_server.db "SELECT name FROM clients;"  # exjang, DESKTOP-KFMQTSH
sudo tail -20 /var/log/urbackup.log                           # 에러 없어야 함
```

---

## 8. 백업 / 롤백 자산

- 데이터 백업(홈): `~/urbackup-migration-backup-*` (특히 `*-VARDIR`가 활성 `/var/urbackup` 전체).
- 바이너리 롤백: `/usr/bin/urbackupsrv.pkg-2.5.38.bak`
- www 롤백: `/usr/share/urbackup.pkg-2.5.38.bak`
- DB를 만지기 전엔 **서버를 멈추고**(WAL flush) 백업할 것:
  ```bash
  sudo systemctl stop urbackupsrv
  sudo tar czf ~/urbackup-var-$(date +%Y%m%d-%H%M%S).tar.gz -C /var urbackup
  ```

### apt 업데이트 주의
`apt`로 `urbackup-server`가 갱신되면 소스 빌드본을 덮어쓴다. 고정하려면:
```bash
sudo apt-mark hold urbackup-server
```

---

## 9. 빠른 체크리스트 (빌드/배포 전)

- [ ] `git branch --show-current` → `2.5.x`
- [ ] `grep -m1 "int max_v=" urbackupserver/dllmain.cpp` ≥ 현재 DB 버전
- [ ] configure에 `--prefix=/usr --localstatedir=/var` 포함
- [ ] 경로 매크로 바꿨으면 `cmdline_preprocessor`/`ClientService` `.o` 삭제 후 재빌드
- [ ] `strings urbackupsrv | grep -E "^/var$"` 로 CWD 매크로 확인
- [ ] 설치 후 `/usr/share/urbackup` 권한 `root:root` 755/644
- [ ] `restart`로 기동, CWD=`/var`, 웹 UI 200, 클라이언트 목록 정상
