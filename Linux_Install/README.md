# SollunaMail Linux Debian (`.deb`) Installer Guide

이 디렉터리는 Linux (Ubuntu / Debian / Linux Mint `amd64`) 환경에서 Qt 6 공유 라이브러리와 플러그인이 모두 번들링된 자립형 **`sollunamail_1.0.0_amd64.deb`** 설치 패키지를 생성하는 스크립트를 포함합니다.

---

## 1. `.deb` 패키지 생성 방법 (`build_deb.sh`)

Release 빌드가 완료된 후 아래 명령을 실행하면 `/opt/Git/SollunaMail/Linux_Install/sollunamail_1.0.0_amd64.deb` 패키지가 생성됩니다.

```bash
cd Linux_Install
./build_deb.sh /opt/Build/SollunaMail/build/Desktop-Release
```
- 인자를 생략하면 기본 경로(`/opt/Build/SollunaMail/build/Desktop-Release`)에서 `SollunaMail` 실행파일을 자동으로 탐색합니다.
- Qt 6 런타임 공유 라이브러리(`.so`), 플랫폼·이미지·TLS·SQL 플러그인, `QtWebEngineProcess` 및 리소스를 `/opt/SollunaMail/` 내부에 자립형(Self-contained)으로 번들링합니다.

---

## 2. 패키지 설치 및 실행 방법

### 설치 (권장 — 시스템 기본 의존성 자동 확인)
```bash
sudo apt update
sudo apt install ./sollunamail_1.0.0_amd64.deb
```

### 실행
- 데스크톱 애플리케이션 메뉴에서 **SollunaMail** 아이콘 클릭
- 또는 터미널에서 실행:
  ```bash
  sollunamail
  ```

### 제거
```bash
sudo apt remove sollunamail
```

---

## 3. 필요 디스크 공간

| 구분 | 용량 |
| :--- | :--- |
| **설치 파일 (`.deb`) 크기** | 약 `158 MB` |
| **설치 후 실제 점유 공간 (`/opt/SollunaMail`)** | 약 `438 MB` |
| **권장 여유 디스크 공간** | 약 `650 MB ~ 1 GB` 이상 |
