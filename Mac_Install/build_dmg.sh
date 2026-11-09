#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$PROJECT_ROOT/SollunaMail"
DMG_NAME="SollunaMail_1.0.0_macOS"
OUTPUT_DMG="$SCRIPT_DIR/${DMG_NAME}.dmg"
STAGE_DIR="/tmp/sollunamail_dmg_stage_$$"
ICONSET_DIR="/tmp/solluna_icon_$$.iconset"

# 1. Qt 6 for macOS 및 CMake 도구 경로 탐색
QT_PREFIX="/Users/icsly0808/Qt/6.12.0/macos"
if [ ! -d "$QT_PREFIX" ]; then
    for cand in /Users/*/Qt/6.*/macos /opt/homebrew/opt/qt /usr/local/opt/qt; do
        if [ -x "$cand/bin/macdeployqt" ]; then
            QT_PREFIX="$cand"
            break
        fi
    done
fi

CMAKE_BIN="/Users/icsly0808/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
if [ ! -x "$CMAKE_BIN" ]; then
    CMAKE_BIN="$(command -v cmake || true)"
fi
MACDEPLOYQT_BIN="$QT_PREFIX/bin/macdeployqt"

# 2. Release 빌드 디렉터리 확인 및 최신 소스 빌드
RELEASE_BUILD_DIR="/Users/icsly0808/Work/Git/Build/Release"
if [ ! -d "$RELEASE_BUILD_DIR" ]; then
    RELEASE_BUILD_DIR="$PROJECT_ROOT/SollunaMail/build/Release"
    mkdir -p "$RELEASE_BUILD_DIR"
fi

echo "============================================================"
echo " SollunaMail macOS Self-Contained (.dmg) 인스톨러 생성 시작"
echo " - Qt 경로        : $QT_PREFIX"
echo " - 빌드 경로      : $RELEASE_BUILD_DIR"
echo " - 출력 DMG 파일  : $OUTPUT_DMG"
echo "============================================================"

if [ -x "$CMAKE_BIN" ]; then
    echo "[1/6] Release 빌드 최신화 수행 중..."
    if [ ! -f "$RELEASE_BUILD_DIR/CMakeCache.txt" ]; then
        "$CMAKE_BIN" -S "$SRC_DIR" -B "$RELEASE_BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_PREFIX_PATH="$QT_PREFIX"
    fi
    "$CMAKE_BIN" --build "$RELEASE_BUILD_DIR" --parallel 8
fi

APP_BUNDLE_SRC="$RELEASE_BUILD_DIR/SollunaMail.app"
if [ ! -d "$APP_BUNDLE_SRC" ]; then
    echo "[오류] 빌드된 SollunaMail.app 번들을 찾을 수 없습니다: $APP_BUNDLE_SRC"
    exit 1
fi

# 3. 스테이징 폴더 준비 및 SollunaMail.app 복사
echo "[2/6] DMG 스테이징 디렉터리 구성 및 앱 번들 복사 중..."
rm -rf "$STAGE_DIR" "$ICONSET_DIR"
mkdir -p "$STAGE_DIR"
cp -R "$APP_BUNDLE_SRC" "$STAGE_DIR/SollunaMail.app"
STAGE_APP="$STAGE_DIR/SollunaMail.app"
# 빌드 폴더 실행 중 생성되었을 수 있는 로컬 테스트 캐시/설정 데이터를 번들에서 제거 (개인정보 유출 및 codesign 오류 방지)
rm -rf "$STAGE_APP/Contents/MacOS/cache" \
       "$STAGE_APP/Contents/MacOS/config_set" \
       "$STAGE_APP/Contents/MacOS/pop3" \
       "$STAGE_APP/Contents/MacOS/"*.json
mkdir -p "$STAGE_APP/Contents/Resources/fonts"

# 4. macOS .icns 고해상도 아이콘 생성 및 리소스/폰트 번들링
echo "[3/6] macOS .icns 아이콘 생성 및 내장 다국어 폰트 번들링 중..."
if [ -f "$SRC_DIR/solluna_icon.png" ]; then
    mkdir -p "$ICONSET_DIR"
    sips -s format png -z 16 16     "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_16x16.png" >/dev/null
    sips -s format png -z 32 32     "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_16x16@2x.png" >/dev/null
    sips -s format png -z 32 32     "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_32x32.png" >/dev/null
    sips -s format png -z 64 64     "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_32x32@2x.png" >/dev/null
    sips -s format png -z 128 128   "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_128x128.png" >/dev/null
    sips -s format png -z 256 256   "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_128x128@2x.png" >/dev/null
    sips -s format png -z 256 256   "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_256x256.png" >/dev/null
    sips -s format png -z 512 512   "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_256x256@2x.png" >/dev/null
    sips -s format png -z 512 512   "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_512x512.png" >/dev/null
    sips -s format png -z 1024 1024 "$SRC_DIR/solluna_icon.png" --out "$ICONSET_DIR/icon_512x512@2x.png" >/dev/null
    iconutil -c icns "$ICONSET_DIR" -o "$STAGE_APP/Contents/Resources/solluna_icon.icns"
    rm -rf "$ICONSET_DIR"
    cp -f "$SRC_DIR/solluna_icon.png" "$STAGE_APP/Contents/Resources/solluna_icon.png"
fi

if [ -d "$SRC_DIR/fonts" ]; then
    cp -rf "$SRC_DIR/fonts/"* "$STAGE_APP/Contents/Resources/fonts/" 2>/dev/null || true
fi

# Info.plist 에 CFBundleIconFile 및 최소 macOS 버전 보장
if [ -f "$STAGE_APP/Contents/Info.plist" ]; then
    /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile solluna_icon.icns" "$STAGE_APP/Contents/Info.plist" 2>/dev/null || \
        /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string solluna_icon.icns" "$STAGE_APP/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c "Set :NSHighResolutionCapable true" "$STAGE_APP/Contents/Info.plist" 2>/dev/null || \
        /usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$STAGE_APP/Contents/Info.plist"
fi

# 5. macdeployqt를 통한 Qt 6.12 프레임워크·플러그인·QtWebEngine 완전 내장
echo "[4/6] macdeployqt 실행 (Qt6 프레임워크, 플러그인, QtWebEngine 번들링)..."
"$MACDEPLOYQT_BIN" "$STAGE_APP" -always-overwrite

# 6. Ad-hoc 코드 서명(codesign) 적용 및 검증
echo "[5/6] macOS 번들 코드 서명(codesign) 및 무결성 검증 중..."
codesign --force --deep --sign - "$STAGE_APP"
codesign --verify --deep --strict "$STAGE_APP"

# 7. DMG 내부에 /Applications 드래그 앤 드롭 심볼릭 링크 및 언인스톨(백업) 스크립트 배치
ln -s /Applications "$STAGE_DIR/Applications"

cat > "$STAGE_DIR/Uninstall_SollunaMail.command" << 'EOF'
#!/bin/bash
echo "============================================================"
echo " SollunaMail macOS 제거 및 사용자 계정·메일 백업 도구"
echo "============================================================"

# 1. 실행 중인 SollunaMail 종료
killall SollunaMail 2>/dev/null || true
killall QtWebEngineProcess 2>/dev/null || true
sleep 1

# 2. 사용자 데이터(~/Library/Application Support/Solluna IoT/SollunaMail) 백업
DATA_ROOT="$HOME/Library/Application Support/Solluna IoT/SollunaMail"
BACKUP_DIR="$DATA_ROOT/Backup"
if [ -d "$DATA_ROOT/config_set" ] || [ -d "$DATA_ROOT/cache" ]; then
    mkdir -p "$BACKUP_DIR"
    cp -rf "$DATA_ROOT/config_set/"*.json "$BACKUP_DIR/" 2>/dev/null || true
    cp -rf "$DATA_ROOT/cache/"*.json "$BACKUP_DIR/" 2>/dev/null || true
    rm -rf "$DATA_ROOT/config_set" "$DATA_ROOT/cache"
    echo " - 기존 계정 및 메일 캐시를 백업 폴더에 안전하게 보관했습니다:"
    echo "   $BACKUP_DIR"
    echo "   (추후 재설치 시 첫 실행 화면에서 백업 복구 여부를 선택할 수 있습니다.)"
fi

# 3. 자동 시작 LaunchAgent 제거
rm -f "$HOME/Library/LaunchAgents/com.solluna.mail.plist"

# 4. /Applications/SollunaMail.app 제거
if [ -d "/Applications/SollunaMail.app" ]; then
    rm -rf "/Applications/SollunaMail.app"
    echo " - /Applications/SollunaMail.app 프로그램이 제거되었습니다."
else
    echo " - /Applications/SollunaMail.app 이 이미 제거되어 있습니다."
fi

echo "============================================================"
echo " 제거가 완료되었습니다. 이 창을 닫으셔도 됩니다."
echo "============================================================"
EOF
chmod 755 "$STAGE_DIR/Uninstall_SollunaMail.command"

# 8. hdiutil을 사용하여 압축 DMG(UDZO) 생성
echo "[6/6] hdiutil로 압축 DMG 이미지 생성 중..."
rm -f "$OUTPUT_DMG"
hdiutil create -volname "SollunaMail" \
    -srcfolder "$STAGE_DIR" \
    -ov -format UDZO \
    "$OUTPUT_DMG"

rm -rf "$STAGE_DIR"

echo "============================================================"
echo " [완료] macOS 설치용 DMG 패키지 생성 성공!"
ls -lh "$OUTPUT_DMG"
echo "============================================================"

