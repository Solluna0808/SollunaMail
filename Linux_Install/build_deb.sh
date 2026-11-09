#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$PROJECT_ROOT/SollunaMail"
PKG_NAME="sollunamail_1.0.0_amd64"
PKG_DIR="$SCRIPT_DIR/$PKG_NAME"
APP_INSTALL_DIR="$PKG_DIR/opt/SollunaMail"
OUTPUT_DEB="$SCRIPT_DIR/${PKG_NAME}.deb"

# 1. Release 실행 파일 경로 탐색 (/opt/Build/SollunaMail/build/Desktop-Release 우선)
RELEASE_BIN="/opt/Build/SollunaMail/build/Desktop-Release/SollunaMail"
DEBUG_BIN="/opt/Build/SollunaMail/build/Desktop-Debug/SollunaMail"

if [ -f "$RELEASE_BIN" ]; then
    BIN_PATH="$RELEASE_BIN"
elif [ -f "$DEBUG_BIN" ]; then
    BIN_PATH="$DEBUG_BIN"
else
    echo "[오류] 빌드된 SollunaMail 실행 파일을 찾을 수 없습니다: $RELEASE_BIN"
    exit 1
fi

echo "============================================================"
echo " SollunaMail Self-Contained Debian (.deb) 패키지 생성 시작"
echo " - 실행 파일 원본 : $BIN_PATH"
echo " - 패키지 출력    : $OUTPUT_DEB"
echo "============================================================"

# 2. 디렉터리 구조 초기화 및 생성
rm -rf "$APP_INSTALL_DIR/lib" "$APP_INSTALL_DIR/plugins" "$APP_INSTALL_DIR/libexec" "$APP_INSTALL_DIR/resources" "$APP_INSTALL_DIR/translations"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$APP_INSTALL_DIR/fonts"
mkdir -p "$APP_INSTALL_DIR/lib"
mkdir -p "$APP_INSTALL_DIR/plugins"
mkdir -p "$APP_INSTALL_DIR/libexec"
mkdir -p "$APP_INSTALL_DIR/resources"
mkdir -p "$APP_INSTALL_DIR/translations"
mkdir -p "$PKG_DIR/usr/share/applications"
mkdir -p "$PKG_DIR/usr/share/pixmaps"
mkdir -p "$PKG_DIR/usr/share/icons/hicolor/256x256/apps"

# 3. 실행 파일, 아이콘, 내장 폰트 복사
cp -f "$BIN_PATH" "$APP_INSTALL_DIR/SollunaMail"
chmod 755 "$APP_INSTALL_DIR/SollunaMail"

cp -f "$SRC_DIR/solluna_icon.png" "$APP_INSTALL_DIR/solluna_icon.png"
cp -f "$SRC_DIR/solluna_icon.png" "$PKG_DIR/usr/share/pixmaps/sollunamail.png"
cp -f "$SRC_DIR/solluna_icon.png" "$PKG_DIR/usr/share/icons/hicolor/256x256/apps/sollunamail.png"

if [ -d "$SRC_DIR/fonts" ]; then
    cp -rf "$SRC_DIR/fonts/"* "$APP_INSTALL_DIR/fonts/" 2>/dev/null || true
fi

# 4. qt.conf 및 실행 런처 스크립트(sollunamail-launcher.sh) 생성
cat > "$APP_INSTALL_DIR/qt.conf" << 'EOF'
[Paths]
Prefix = /opt/SollunaMail
Libraries = lib
Plugins = plugins
LibraryExecutables = libexec
Data = .
Translations = translations
EOF

cat > "$APP_INSTALL_DIR/sollunamail-launcher.sh" << 'EOF'
#!/bin/sh
APP_DIR="/opt/SollunaMail"
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APP_DIR/plugins"
export QTWEBENGINEPROCESS_PATH="$APP_DIR/libexec/QtWebEngineProcess"
export QTWEBENGINE_LOCALES_PATH="$APP_DIR/translations/qtwebengine_locales"
exec "$APP_DIR/SollunaMail" "$@"
EOF
chmod 755 "$APP_INSTALL_DIR/sollunamail-launcher.sh"

# 5. Qt6 필수 플러그인(platforms, tls, imageformats, iconengines, xcbglintegrations, wayland 등) 복사
QT_PLUGIN_SRC="/usr/lib/x86_64-linux-gnu/qt6/plugins"
for plug_subdir in platforms tls imageformats iconengines xcbglintegrations wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration platformthemes platforminputcontexts networkinformation generic; do
    if [ -d "$QT_PLUGIN_SRC/$plug_subdir" ]; then
        cp -rf "$QT_PLUGIN_SRC/$plug_subdir" "$APP_INSTALL_DIR/plugins/"
    fi
done

# 6. Chromium QtWebEngineProcess 및 리소스(.pak, v8_context_snapshot.bin, locales) 복사
if [ -f "/usr/lib/qt6/libexec/QtWebEngineProcess" ]; then
    cp -f "/usr/lib/qt6/libexec/QtWebEngineProcess" "$APP_INSTALL_DIR/libexec/QtWebEngineProcess"
    chmod 755 "$APP_INSTALL_DIR/libexec/QtWebEngineProcess"
fi

if [ -d "/usr/share/qt6/resources" ]; then
    cp -rf /usr/share/qt6/resources/* "$APP_INSTALL_DIR/resources/"
fi

if [ -d "/usr/share/qt6/translations/qtwebengine_locales" ]; then
    cp -rf "/usr/share/qt6/translations/qtwebengine_locales" "$APP_INSTALL_DIR/translations/"
fi

# 7. ldd를 이용하여 SollunaMail + QtWebEngineProcess + 모든 Qt 플러그인의 공유 라이브러리 자동 수집
echo " - Qt6 및 런타임 의존 공유 라이브러리(.so) 수집 중..."
is_excluded_host_lib() {
    case "$1" in
        ld-linux*.so*|linux-vdso.so*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libnsl.so.*|libanl.so.*|libBrokenLocale.so.*)
            return 0 ;;
        libstdc++.so.*|libgcc_s.so.*)
            return 0 ;;
        libGL.so.*|libEGL.so.*|libGLX.so.*|libOpenGL.so.*|libGLdispatch.so.*|libgbm.so.*|libdrm*.so.*)
            return 0 ;;
        libX11.so.*|libX11-xcb.so.*|libxcb.so.*|libXau.so.*|libXdmcp.so.*|libXext.so.*|libdbus-1.so.*|libsystemd.so.*|libudev.so.*|libasound.so.*|libfontconfig.so.*|libfreetype.so.*|libexpat.so.*|libz.so.*)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

TARGET_BINS=("$APP_INSTALL_DIR/SollunaMail")
if [ -f "$APP_INSTALL_DIR/libexec/QtWebEngineProcess" ]; then
    TARGET_BINS+=("$APP_INSTALL_DIR/libexec/QtWebEngineProcess")
fi
while IFS= read -r -d '' sofile; do
    TARGET_BINS+=("$sofile")
done < <(find "$APP_INSTALL_DIR/plugins" -type f -name "*.so" -print0)

ldd "${TARGET_BINS[@]}" 2>/dev/null | awk '/=> \// {print $3}' | sort -u | while read -r libpath; do
    [ -f "$libpath" ] || continue
    libname="$(basename "$libpath")"
    if ! is_excluded_host_lib "$libname"; then
        cp -Lf "$libpath" "$APP_INSTALL_DIR/lib/$libname"
    fi
done

# OpenSSL 3 런타임이 플러그인에서 동적 로드(dlopen)될 수 있으므로 함께 복사
for ssl_lib in /usr/lib/x86_64-linux-gnu/libssl.so.3 /usr/lib/x86_64-linux-gnu/libcrypto.so.3; do
    if [ -f "$ssl_lib" ]; then
        cp -Lf "$ssl_lib" "$APP_INSTALL_DIR/lib/$(basename "$ssl_lib")"
    fi
done

LIB_COUNT=$(ls -1 "$APP_INSTALL_DIR/lib" | wc -l)
echo " - 총 ${LIB_COUNT}개의 공유 라이브러리 번들링 완료"

# 8. Installed-Size (KB) 자동 계산 및 DEBIAN/control 갱신
INSTALLED_KB=$(du -sk "$PKG_DIR/opt" "$PKG_DIR/usr" | awk '{sum += $1} END {print sum}')
sed -i "s/^Installed-Size:.*/Installed-Size: ${INSTALLED_KB}/" "$PKG_DIR/DEBIAN/control"

# 9. DEBIAN 제어 파일 및 디렉터리 표준 권한 설정 (dpkg-deb 필수 조건: 0755 / 0644)
find "$PKG_DIR" -type d -exec chmod 755 {} +
find "$PKG_DIR/opt" "$PKG_DIR/usr" -type f -exec chmod 644 {} +
chmod 755 "$APP_INSTALL_DIR/SollunaMail"
chmod 755 "$APP_INSTALL_DIR/sollunamail-launcher.sh"
if [ -f "$APP_INSTALL_DIR/libexec/QtWebEngineProcess" ]; then
    chmod 755 "$APP_INSTALL_DIR/libexec/QtWebEngineProcess"
fi
chmod 644 "$PKG_DIR/DEBIAN/control"
chmod 755 "$PKG_DIR/DEBIAN/preinst" "$PKG_DIR/DEBIAN/postinst" "$PKG_DIR/DEBIAN/prerm" "$PKG_DIR/DEBIAN/postrm"

# 10. dpkg-deb 빌드 수행
dpkg-deb --root-owner-group --build "$PKG_DIR" "$OUTPUT_DEB"

echo "============================================================"
echo " [완료] Self-Contained Debian 패키지 생성 성공!"
ls -lh "$OUTPUT_DEB"
echo "============================================================"
