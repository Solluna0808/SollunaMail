# SollunaMail

**SollunaMail**은 Qt 6 (C++17) 기반의 크로스플랫폼(Windows · Linux · macOS) 통합 이메일·캘린더·주소록 클라이언트입니다.

---

## 주요 기능 (Key Features)

- **멀티 계정 통합 메일함 (Unified Inbox):**
  - Google (Gmail OAuth 2.0 & 16자리 앱 비밀번호), Apple iCloud, 네이버(Naver), 다음·카카오(Daum/Kakao), Microsoft Outlook 및 일반 회사/도메인 IMAP·POP3·SMTP 계정 지원
  - 보내는 메일 서버(SMTP) 분리 설정 지원 (Mailtrap, Mailgun, SendGrid, Custom SMTP)
- **실시간 메일 수신 및 메일 관리:**
  - IMAP IDLE 실시간 푸시 및 백그라운드 주기 동기화
  - POP3 로컬 메일함 영구 보관(`pop3/`) 및 서버 복사본 유지 옵션
  - 예약 발송, 수신 확인(MDN), 추적 픽셀 차단(Privacy Hub), 발신자 차단 및 스팸 관리
- **캘린더 & 주소록 통합 연동:**
  - Google Calendar / Apple iCloud CalDAV 양방향 동기화 및 로컬 일정·공휴일 표시
  - Google Contacts / iCloud CardDAV 주소록 동기화 및 자동 완성
- **다국어 & 테마 지원:**
  - 한국어(🇰🇷) · 영어(🇺🇸) · 일본어(🇯🇵) 실시간 전환
  - 다크 모드 / 라이트 모드 및 다양한 강조 색상 테마 지원
- **AI & MCP (Model Context Protocol) 연동:**
  - Gemini, Claude, Codex(ChatGPT) 기반 이메일 요약·번역·답장 초안 작성 및 `--mcp-stdio` 서버 내장

---

## 디렉터리 구조

```text
SollunaMail/
├── SollunaMail/                  # Qt 6 C++ 애플리케이션 소스 코드 및 리소스
│   ├── CMakeLists.txt            # CMake 빌드 설정
│   ├── oauth_secrets_local.h.example # Google OAuth 2.0 클라이언트 키 설정 템플릿
│   ├── main.cpp
│   ├── mainwindow.h / .cpp / _part2.cpp / .ui
│   ├── googleprovider.h / .cpp
│   ├── settingsdialog.h / .cpp
│   ├── composedialog.h / .cpp
│   ├── customwidgets.h / .cpp
│   ├── mailmodels.h
│   └── mailtheme.h / .cpp
└── Linux_Install/                # Linux Debian (.deb) 자립형 설치 패키지 생성 도구
    ├── build_deb.sh              # Qt 6 런타임 포함 .deb 패키지 자동 빌드 스크립트
    └── README.md                 # 리눅스 패키지 생성 및 설치 안내서
```

---

## 빌드 방법 (Build Instructions)

### 1. 필수 환경
- **Qt 6.5 이상** (`Core`, `Gui`, `Widgets`, `Network`, `LinguistTools`, 선택: `WebEngineWidgets`)
- **CMake 3.19 이상** 및 C++17 호환 컴파일러 (GCC / Clang / MSVC 2022)

### 2. Google OAuth 2.0 클라이언트 키 설정 (선택 사항)
보안을 위해 실제 Google OAuth 2.0 Client ID / Secret 헤더(`oauth_secrets_local.h`)는 저장소에 포함되지 않습니다.
내장 Google 브라우저 로그인을 사용하려면 템플릿 파일을 복사하여 키를 설정하세요:

```bash
cp SollunaMail/oauth_secrets_local.h.example SollunaMail/oauth_secrets_local.h
```
또는 실행 중 **[환경 설정 → 고급 설정]** 화면에서 사용자 지정 Google Client ID / Secret을 입력하거나 Google 16자리 앱 비밀번호로 연동할 수 있습니다.

### 3. CMake 빌드
```bash
cmake -S SollunaMail -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### 4. Linux Debian (`.deb`) 설치 패키지 생성
Linux Release 빌드 완료 후 아래 스크립트를 실행하면 Qt 6 공유 라이브러리와 플러그인이 모두 번들링된 자립형 `.deb` 설치 파일이 생성됩니다:

```bash
cd Linux_Install
./build_deb.sh /path/to/Release/build/dir
```
자세한 내용은 [`Linux_Install/README.md`](Linux_Install/README.md)를 참고하세요.
