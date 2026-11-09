/* SollunaMail Install Script (Qt IFW 4.10)
   - 바로가기: 바탕화면, 시작 메뉴
   - 기존 설정(config_set/accounts.json, app_settings.json) 및 캐시(cache/*.json)를 %APPDATA%/SOLLUNA_MAIL/Backup에 자동 백업
   - 언인스톨 시 실행 중인 SollunaMail.exe 자동 종료 및 런타임 생성 폴더(config_set, cache, pop3) 완전 정리로 Rmdir 오류 방지
   - 대상: Windows 전용
*/

console.log("I_DEBUG: installscripts.qs LOADED");

function Component() {
  var comp = installer.componentByName('com.sollunamail.app');
  console.log("I_DEBUG: COMPONENT NAME = " + (comp ? comp.name : "<null>"));
  if (!comp) { console.log("I_ERROR: com.sollunamail.app not found"); return; }

  try {
    var ok1 = installer.addWizardPage(comp, "PerUserMachinePage", QInstaller.TargetDirectory);
    console.log("DEBUG: addWizardPage PerUserMachinePage @TargetDirectory -> " + ok1);
  } catch (e1) { console.log("Z_ERROR11: " + e1); }

  try {
    var ok2 = installer.addWizardPage(comp, "LaunchOptionPage", QInstaller.ReadyForInstallation);
    console.log("DEBUG: addWizardPage LaunchOptionPage @ReadyForInstallation -> " + ok2);
  } catch (e2) { console.log("Z_ERROR12: " + e2); }
}

/* ---------------- 설치/언인스톨 동작 ---------------- */
Component.prototype.createOperations = function () {
  // 경로는 / 사용
  var exe   = "@TargetDir@/SollunaMail.exe";
  var ico   = "@TargetDir@/solluna_icon.ico";
  
  var appDataDir = userAppDataDir();
  var backupDir  = appDataDir + "/Backup";
  var targetDirCmd     = "@TargetDir@".replace(/\//g, "\\");
  var targetConfigCmd  = targetDirCmd + "\\config_set";
  var targetCacheCmd   = targetDirCmd + "\\cache";
  var appDataConfigCmd = (appDataDir + "/config_set").replace(/\//g, "\\");
  var appDataCacheCmd  = (appDataDir + "/cache").replace(/\//g, "\\");
  var backupDirCmd     = backupDir.replace(/\//g, "\\");
  var startupLinkCmd   = ((installer.environmentVariable("APPDATA") || "")
                         + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\SollunaMail.lnk").replace(/\//g, "\\");
  var isUpgrade        = (installer.value("UpgradeMode") === "1");

  var copyToBackupCmd =
      "mkdir \"" + backupDirCmd + "\" >nul 2>&1"
    + " & if exist \"" + targetConfigCmd + "\\accounts.json\" copy /Y \"" + targetConfigCmd + "\\accounts.json\" \"" + backupDirCmd + "\\accounts.json\" >nul 2>&1"
    + " & if exist \"" + targetConfigCmd + "\\app_settings.json\" copy /Y \"" + targetConfigCmd + "\\app_settings.json\" \"" + backupDirCmd + "\\app_settings.json\" >nul 2>&1"
    + " & if exist \"" + targetCacheCmd + "\\conversations_cache.json\" copy /Y \"" + targetCacheCmd + "\\conversations_cache.json\" \"" + backupDirCmd + "\\conversations_cache.json\" >nul 2>&1"
    + " & if exist \"" + targetCacheCmd + "\\deleted_mails_cache.json\" copy /Y \"" + targetCacheCmd + "\\deleted_mails_cache.json\" \"" + backupDirCmd + "\\deleted_mails_cache.json\" >nul 2>&1"
    + " & if exist \"" + targetCacheCmd + "\\calendars_cache.json\" copy /Y \"" + targetCacheCmd + "\\calendars_cache.json\" \"" + backupDirCmd + "\\calendars_cache.json\" >nul 2>&1"
    + " & if exist \"" + targetCacheCmd + "\\contacts_cache.json\" copy /Y \"" + targetCacheCmd + "\\contacts_cache.json\" \"" + backupDirCmd + "\\contacts_cache.json\" >nul 2>&1"
    + " & if exist \"" + targetCacheCmd + "\\contact_groups_cache.json\" copy /Y \"" + targetCacheCmd + "\\contact_groups_cache.json\" \"" + backupDirCmd + "\\contact_groups_cache.json\" >nul 2>&1"
    + " & if exist \"" + appDataConfigCmd + "\\accounts.json\" copy /Y \"" + appDataConfigCmd + "\\accounts.json\" \"" + backupDirCmd + "\\accounts.json\" >nul 2>&1"
    + " & if exist \"" + appDataConfigCmd + "\\app_settings.json\" copy /Y \"" + appDataConfigCmd + "\\app_settings.json\" \"" + backupDirCmd + "\\app_settings.json\" >nul 2>&1"
    + " & if exist \"" + appDataCacheCmd + "\\conversations_cache.json\" copy /Y \"" + appDataCacheCmd + "\\conversations_cache.json\" \"" + backupDirCmd + "\\conversations_cache.json\" >nul 2>&1"
    + " & if exist \"" + appDataCacheCmd + "\\deleted_mails_cache.json\" copy /Y \"" + appDataCacheCmd + "\\deleted_mails_cache.json\" \"" + backupDirCmd + "\\deleted_mails_cache.json\" >nul 2>&1"
    + " & if exist \"" + appDataCacheCmd + "\\calendars_cache.json\" copy /Y \"" + appDataCacheCmd + "\\calendars_cache.json\" \"" + backupDirCmd + "\\calendars_cache.json\" >nul 2>&1"
    + " & if exist \"" + appDataCacheCmd + "\\contacts_cache.json\" copy /Y \"" + appDataCacheCmd + "\\contacts_cache.json\" \"" + backupDirCmd + "\\contacts_cache.json\" >nul 2>&1"
    + " & if exist \"" + appDataCacheCmd + "\\contact_groups_cache.json\" copy /Y \"" + appDataCacheCmd + "\\contact_groups_cache.json\" \"" + backupDirCmd + "\\contact_groups_cache.json\" >nul 2>&1";

  var backupAndResetCmd =
      copyToBackupCmd
    + " & del /F /Q \"" + targetConfigCmd + "\\*.json\" \"" + targetConfigCmd + "\\.backup_prompt_done\" >nul 2>&1"
    + " & del /F /Q \"" + targetCacheCmd + "\\*.json\" >nul 2>&1"
    + " & del /F /Q \"" + appDataConfigCmd + "\\*.json\" \"" + appDataConfigCmd + "\\.backup_prompt_done\" >nul 2>&1"
    + " & del /F /Q \"" + appDataCacheCmd + "\\*.json\" >nul 2>&1";

  var upgradePreserveCmd =
      copyToBackupCmd
    + " & mkdir \"" + targetConfigCmd + "\" >nul 2>&1"
    + " & mkdir \"" + targetCacheCmd + "\" >nul 2>&1"
    + " & echo 1 > \"" + targetConfigCmd + "\\.backup_prompt_done\""
    + " & if exist \"" + appDataConfigCmd + "\" echo 1 > \"" + appDataConfigCmd + "\\.backup_prompt_done\"";

  var installPrepCmd =
      "taskkill /F /IM SollunaMail.exe /T >nul 2>&1"
    + " & taskkill /F /IM QtWebEngineProcess.exe /T >nul 2>&1"
    + " & " + (isUpgrade ? upgradePreserveCmd : backupAndResetCmd)
    + " & mkdir \"" + targetConfigCmd + "\" >nul 2>&1"
    + " & mkdir \"" + targetCacheCmd + "\" >nul 2>&1"
    + " & del /F /Q \"" + startupLinkCmd + "\" >nul 2>&1";

  var uninstallCleanupCmd =
      "taskkill /F /IM SollunaMail.exe /T >nul 2>&1"
    + " & taskkill /F /IM QtWebEngineProcess.exe /T >nul 2>&1"
    + " & " + backupAndResetCmd
    + " & rmdir /S /Q \"" + targetConfigCmd + "\" >nul 2>&1"
    + " & rmdir /S /Q \"" + targetCacheCmd + "\" >nul 2>&1"
    + " & rmdir /S /Q \"" + targetDirCmd + "\\pop3\" >nul 2>&1"
    + " & rmdir /S /Q \"" + appDataConfigCmd + "\" >nul 2>&1"
    + " & rmdir /S /Q \"" + appDataCacheCmd + "\" >nul 2>&1"
    + " & reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\" /v \"SollunaMail\" /f >nul 2>&1"
    + " & del /F /Q \"" + startupLinkCmd + "\" >nul 2>&1";

  if (installer.isInstaller()) {
    // 1. 아카이브(.7z) 압축 해제(component.createOperations) 전에 먼저 실행 중인 프로세스(SollunaMail.exe, QtWebEngineProcess.exe)를 종료하고 기존 설정을 Backup으로 보존!
    component.addOperation("Execute", "cmd.exe", "/C", installPrepCmd);

    // 2. 본 패키지 아카이브(.7z) 압축 해제
    component.createOperations();

    // 3. 바로가기 (바탕화면 & 시작 메뉴)
    component.addOperation("CreateShortcut", exe, "@DesktopDir@/SollunaMail.lnk",
      "iconPath=" + ico, "description=SollunaMail", "workingDirectory=@TargetDir@");
    component.addOperation("CreateShortcut", exe, "@StartMenuDir@/SollunaMail.lnk",
      "iconPath=" + ico, "description=SollunaMail", "workingDirectory=@TargetDir@");

    // 4. 언인스톨(UNDO) 시 프로세스 종료·백업·런타임 폴더 완전 삭제가 가장 먼저 실행되도록 마지막에 UNDO 등록
    component.addOperation(
      "Execute", "cmd.exe", "/C", "ver >nul",
      "UNDOEXECUTE", "cmd.exe", "/C", uninstallCleanupCmd
    );

  } else if (installer.isUninstaller()) {
    // ===== 언인스톨 (프로세스 종료·백업·폴더 정리를 먼저 수행 후 기본 언인스톨 진행) =====
    component.addOperation("Execute", "cmd.exe", "/C", uninstallCleanupCmd);
    component.createOperations();
    console.log("DEBUG: Uninstall cleanup (DIRECT) scheduled");
  }
};

/* ---------------- 유틸 ---------------- */
function isWindows() { return true; }

function userAppDataDir() {
  var appdata = (installer.environmentVariable("APPDATA") || "").replace(/\\/g, "/");
  return appdata + "/SOLLUNA_MAIL";
}
