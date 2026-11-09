#!/usr/bin/env bash
# =============================================================================
# SollunaMail — GitHub 전용 클린 폴더(/opt/Git/SollunaMail/Github) 동기화 스크립트
# - 보안 파일(oauth_secrets_local.h, 개인 설정/캐시) 및 대용량 파일(*.deb, pkg_root)을
#   100% 원천 배제하고, 순수 소스 코드와 패키징 스크립트만 선별 복사합니다.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
GITHUB_DIR="${SCRIPT_DIR}"

echo "=================================================================="
echo " [SollunaMail] GitHub 업로드용 클린 소스 동기화 시작"
echo " - 원본 경로 : ${PROJECT_ROOT}"
echo " - 대상 경로 : ${GITHUB_DIR}"
echo "=================================================================="

mkdir -p "${GITHUB_DIR}/SollunaMail"
mkdir -p "${GITHUB_DIR}/Linux_Install"

# 1. 화이트리스트(Whitelist) 기반 SollunaMail 핵심 소스 및 리소스만 복사
SOURCE_FILES=(
    "CMakeLists.txt"
    "main.cpp"
    "mainwindow.h"
    "mainwindow.cpp"
    "mainwindow_part2.cpp"
    "mainwindow.ui"
    "mailmodels.h"
    "mailtheme.h"
    "mailtheme.cpp"
    "customwidgets.h"
    "customwidgets.cpp"
    "composedialog.h"
    "composedialog.cpp"
    "settingsdialog.h"
    "settingsdialog.cpp"
    "googleprovider.h"
    "googleprovider.cpp"
    "resources.qrc"
    "solluna_icon.png"
    "solluna_icon.ico"
    "app_icon.rc"
    "SollunaMail_ko_KR.ts"
    "oauth_secrets_local.h.example"
)

COPIED_COUNT=0
for f in "${SOURCE_FILES[@]}"; do
    if [[ -f "${PROJECT_ROOT}/SollunaMail/${f}" ]]; then
        cp -f "${PROJECT_ROOT}/SollunaMail/${f}" "${GITHUB_DIR}/SollunaMail/${f}"
        COPIED_COUNT=$((COPIED_COUNT + 1))
    else
        echo " [경고] 파일 없음 (건너뜀): SollunaMail/${f}"
    fi
done

# macOS 아이콘이 있을 경우 함께 복사
if [[ -f "${PROJECT_ROOT}/SollunaMail/solluna_icon.icns" ]]; then
    cp -f "${PROJECT_ROOT}/SollunaMail/solluna_icon.icns" "${GITHUB_DIR}/SollunaMail/solluna_icon.icns"
    COPIED_COUNT=$((COPIED_COUNT + 1))
fi

# 2. Linux_Install 폴더는 스크립트(build_deb.sh)와 문서(README.md)만 복사 (.deb 및 pkg_root 절대 제외)
cp -f "${PROJECT_ROOT}/Linux_Install/build_deb.sh" "${GITHUB_DIR}/Linux_Install/build_deb.sh"
chmod 755 "${GITHUB_DIR}/Linux_Install/build_deb.sh"
if [[ -f "${PROJECT_ROOT}/Linux_Install/README.md" ]]; then
    cp -f "${PROJECT_ROOT}/Linux_Install/README.md" "${GITHUB_DIR}/Linux_Install/README.md"
fi

# 3. 혹시라도 실수로 들어갔을 수 있는 보안/대용량 파일 강제 삭제 (이중 안전장치)
rm -f "${GITHUB_DIR}/oauth_secrets_local.h"
rm -f "${GITHUB_DIR}/SollunaMail/oauth_secrets_local.h"
rm -rf "${GITHUB_DIR}/Linux_Install/pkg_root"
rm -f "${GITHUB_DIR}/Linux_Install/"*.deb

# 4. 보안 및 용량 자동 검증
echo "------------------------------------------------------------------"
echo " [검증 1] 복사된 소스 파일 수 : ${COPIED_COUNT} 개 (+ Linux_Install 2개)"
if find "${GITHUB_DIR}" -name "oauth_secrets_local.h" | grep -q .; then
    echo " [오류] 보안 파일(oauth_secrets_local.h)이 감지되었습니다! 중단합니다."
    exit 1
else
    echo " [검증 2] 보안 체크 통과 : oauth_secrets_local.h 포함되지 않음 (안전)"
fi

LARGE_FILES="$(find "${GITHUB_DIR}" -path "${GITHUB_DIR}/.git" -prune -o -type f -size +50M -print)"
if [[ -n "${LARGE_FILES}" ]]; then
    echo " [오류] 50MB 초과 대용량 파일이 감지되었습니다:"
    echo "${LARGE_FILES}"
    exit 1
else
    echo " [검증 3] 용량 체크 통과 : 50MB 초과 파일 없음"
fi

TOTAL_SIZE="$(du -sh --exclude=.git "${GITHUB_DIR}" | awk '{print $1}')"
echo " [검증 4] Github 폴더 전체 순수 용량 : ${TOTAL_SIZE}"
echo "=================================================================="
echo " 동기화 완료! 아래 명령어로 GitHub(Solluna0808/SollunaMail)에 올리실 수 있습니다:"
echo ""
echo "   cd ${GITHUB_DIR}"
echo "   git init -b main"
echo "   git add ."
echo "   git commit -m \"Initial release of SollunaMail\""
echo "   git remote add origin https://github.com/Solluna0808/SollunaMail.git"
echo "   git push -u origin main"
echo "=================================================================="
