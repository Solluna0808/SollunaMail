// controller.qs — IFW 4.x / Qt 6.x

// ------------ helpers ------------
function log(s){ try { console.log(s); } catch(e){} }

function mb_confirm(title, text) {
  try {
    // Yes/No 확인창
    var ret = QMessageBox["question"](gui, title, text,
                QMessageBox["Yes"] | QMessageBox["No"], QMessageBox["No"]);
    return ret === QMessageBox["Yes"];
  } catch (e) {
    // 아주 구형 환경 대비
    return true;
  }
}

function mb_warn(title, text) {
  try { QMessageBox["warning"](gui, title, text, QMessageBox["Ok"]); } catch (e) {}
}

function mb_info(title, text) {
  try { QMessageBox["information"](gui, title, text, QMessageBox["Ok"]); } catch (e) {}
}


function isAdminNow() {
  try {
    if (typeof installer.isAdminRightsAcquired !== "undefined")
      return installer.isAdminRightsAcquired;
    if (typeof installer.isAdmin === "function")
      return installer.isAdmin();
  } catch(e){}
  return false;
}

function _logCandidate(tag, p) {
  try {
    console.log("PATH CANDIDATE[" + tag + "]: " + (p || "<empty>"));
    if (!p) return "";
    var fi = new QFileInfo(p);
    var ok = fi.exists() && fi.isFile();
    console.log("  -> exists=" + ok + ", abs=" + fi.absoluteFilePath());
    return ok ? fi.absoluteFilePath() : "";
  } catch (e) {
    console.log("  -> check error: " + e);
    return "";
  }
}


function findAndSetInstallerPath() {
  var installerName = "SollunaMail_Installer.exe"; 
  var storedPath = "";
  
  // 1. installer.binaryPath 시도
  if (installer.binaryPath && installer.binaryPath.length > 0) {
    storedPath = installer.binaryPath;
  }
  
  // 2. QCoreApplication::applicationFilePath() 시도 및 경로 보정
  if (!storedPath && typeof QCoreApplication !== "undefined") {
    try {
      var p = QCoreApplication["applicationFilePath"](); 
      if (p && p.length > 0) {
        // 경로에 파일 이름이 이미 포함되어 있으면 그대로 사용
        if (p.toLowerCase().indexOf(installerName.toLowerCase()) !== -1) {
            storedPath = p;
        } else {
            // 경로 보정: 문자열 조작으로 디렉토리 추출
            var normalizedPath = p.replace(/\\/g, "/"); 
            var parts = normalizedPath.split("/"); 
            parts.pop(); 
            var dir = parts.join("/");
            storedPath = dir + "/" + installerName;
        }
      }
    } catch (e) {
      log("WARN: findAndSetInstallerPath: QCoreApplication call failed: " + e);
    }
  }

  // 3. (Fallback) 현재 작업 디렉토리 + 하드코딩된 이름
  if (!storedPath) {
      if (typeof installer.cwd === "function") {
          storedPath = installer.cwd() + "/" + installerName;
      }
  }

  // 최종적으로 찾은 경로를 "InstallerPath" 변수에 저장
  if (storedPath) {
    installer.setValue("InstallerPath", storedPath);
    log("DEBUG: InstallerPath set early to: " + storedPath);
  } else {
    installer.setValue("InstallerPath", "");
    log("WARN: InstallerPath could not be set early (empty path).");
  }
}

function exePathSafe() {
  var installerName = "SollunaMail_Installer.exe"; 
  
  // 1. installer.binaryPath (1차 시도)
  if (installer.binaryPath && installer.binaryPath.length > 0) {
    log("DEBUG: exePathSafe: Found via installer.binaryPath: " + installer.binaryPath);
    return installer.binaryPath;
  }
  
  // 2. QCoreApplication::applicationFilePath() 시도 및 경로 보정 (가장 강력한 시도)
  if (typeof QCoreApplication !== "undefined") {
    try {
      var p = QCoreApplication["applicationFilePath"](); 
      if (p && p.length > 0) {
        log("DEBUG: exePathSafe: ApplicationFilePath found: " + p);

        // A. 경로에 파일 이름이 이미 포함되어 있으면 그대로 사용
        if (p.toLowerCase().indexOf(installerName.toLowerCase()) !== -1) {
            return p;
        }

        // B. 경로 보정: 문자열 조작으로 디렉토리 추출
        var normalizedPath = p.replace(/\\/g, "/"); 
        var parts = normalizedPath.split("/"); 
        parts.pop(); 
        var dir = parts.join("/");
        var correctedPath = dir + "/" + installerName;
        
        log("DEBUG: exePathSafe: Path corrected via string manipulation to: " + correctedPath);
        return correctedPath;
      }
    } catch (e) {
      log("WARN: exePathSafe: QCoreApplication call failed: " + e);
    }
  }
  
  // 3. 공격적인 폴백: CWD + Hardcoded Name (최후의 수단)
  if (typeof installer.cwd === "function") {
      var cwdPath = installer.cwd() + "/" + installerName;
      log("DEBUG: exePathSafe: Aggressive Fallback using CWD: " + cwdPath);
      return cwdPath;
  }
  
  // 4. LauncherName 시도 (최후의 방어)
  var ln = installer.value("LauncherName");
  if (ln && ln.length > 0) {
      log("DEBUG: exePathSafe: Found via LauncherName (raw): " + ln);
      return ln;
  }

  // 최종 실패
  log("CRITICAL1: exePathSafe: Could not find any binary path (All methods failed).");
  return ""; 
}


function quotePS(s) { return s.replace(/'/g, "''"); }

function relaunchElevatedAndExit(extraArgs) {
  var bin = exePathSafe();
  if (!bin) {
    return; // 종료하지 않음
  }

  function quotePS(s){ return s.replace(/'/g,"''"); }

  var args = ["--verbose"];
  if (extraArgs && extraArgs.length) args = args.concat(extraArgs);

  // A) PowerShell
  var ps = "Start-Process -Verb RunAs -FilePath '" + quotePS(bin) +
           "' -ArgumentList '" + quotePS(args.join(" ")) + "'";
  console.log("ELEVATE(A): " + ps);
  var ok = false;
  try {
    ok = installer.executeDetached("powershell.exe",
      ["-NoProfile","-ExecutionPolicy","Bypass","-Command", ps]);
  } catch (e) { console.log("ELEVATE(A) error: " + e); }

  // B) mshta 대체 루트
  if (!ok) {
    var vb = 'mshta.exe "javascript:var sh=new ActiveXObject(' +
             '\'Shell.Application\');sh.ShellExecute(\'' +
             bin.replace(/\\/g,'\\\\').replace(/\'/g,"\\'") +
             '\',\'--verbose\',\'\',\'runas\',1);close();"';
    console.log("ELEVATE(B): " + vb);
    try { ok = installer.executeDetached("cmd.exe", ["/C", vb]); }
    catch (e) { console.log("ELEVATE(B) error: " + e); }
  }

  if (ok) {
    installer.setAutomaticAnswer("cancelInstallation", "Yes");
    installer.setCanceled();    // 성공 시에만 종료
  } else {
    mb_warn("권한 승격 실패", "관리자 권한으로 재실행하지 못했습니다. Per-User 설치로 계속 진행합니다.");
  }
}


function resolveTargetDirPath(rawDir) {
  if (!rawDir) return "";
  var p = rawDir;
  var home = (installer.value("HomeDir") || installer.environmentVariable("USERPROFILE") || "").replace(/\\/g, "/");
  var apps = (installer.value("ApplicationsDir") || installer.environmentVariable("ProgramFiles") || "C:/Program Files").replace(/\\/g, "/");
  p = p.replace(/@HomeDir@/g, home).replace(/@ApplicationsDir@/g, apps);
  return p.replace(/\\/g, "/");
}

var g_promptedUpgradeDirs = {};

function prepareTargetDirForUpgrade(rawTargetDir) {
  if (!installer.isInstaller()) return;
  var resolved = resolveTargetDirPath(rawTargetDir);
  if (!resolved) return;

  var hasExe   = installer.fileExists(resolved + "/SollunaMail.exe");
  var hasComp  = installer.fileExists(resolved + "/components.xml");
  var hasMaint = installer.fileExists(resolved + "/SollunaMailMaintenanceTool.exe");

  if (!hasExe && !hasComp && !hasMaint) {
    return;
  }

  if (!g_promptedUpgradeDirs[resolved]) {
    g_promptedUpgradeDirs[resolved] = true;
    var ret = QMessageBox["question"](
      gui,
      "기존 설치 감지 — 업그레이드(Upgrade) 안내",
      "선택하신 경로에 이미 SollunaMail이 설치되어 있습니다.\n" +
      "(" + resolved + ")\n\n" +
      "• [예(Yes) — 업그레이드 설치 (권장)]:\n" +
      "   기존 계정·환경설정·메일/캘린더/주소록 데이터를 그대로 유지하고\n" +
      "   프로그램 실행 파일만 최신 버전으로 업그레이드합니다.\n\n" +
      "• [아니오(No) — 초기화 후 재설치]:\n" +
      "   기존 데이터를 Backup 폴더로 백업한 뒤 초기 상태로 새로 설치합니다.\n\n" +
      "기존 계정과 설정을 유지하며 업그레이드하시겠습니까?",
      QMessageBox["Yes"] | QMessageBox["No"],
      QMessageBox["Yes"]
    );
    var keepData = (ret === QMessageBox["Yes"]);
    installer.setValue("UpgradeMode", keepData ? "1" : "0");
    log("DEBUG: Existing installation detected at " + resolved + " -> UpgradeMode=" + installer.value("UpgradeMode"));
  }

  // 1) 실행 중인 프로세스 직접 종료 (따옴표 불필요)
  try { installer.execute("taskkill.exe", ["/F", "/IM", "SollunaMail.exe", "/T"]); } catch (e) {}
  try { installer.execute("taskkill.exe", ["/F", "/IM", "QtWebEngineProcess.exe", "/T"]); } catch (e) {}
  try { installer.execute("taskkill.exe", ["/F", "/IM", "SollunaMailMaintenanceTool.exe", "/T"]); } catch (e) {}

  // 2) Qt IFW 내장 Delete 연산으로 잠금 메타파일 즉시 삭제 (cmd.exe 따옴표 이스케이프 문제 원천 배제)
  var lockFiles = [
    resolved + "/SollunaMailMaintenanceTool.exe",
    resolved + "/SollunaMailMaintenanceTool.dat",
    resolved + "/SollunaMailMaintenanceTool.ini",
    resolved + "/components.xml",
    resolved + "/InstallationLog.txt",
    resolved + "/installer.dat",
    resolved + "/network.xml"
  ];
  for (var i = 0; i < lockFiles.length; ++i) {
    try {
      if (installer.fileExists(lockFiles[i])) {
        installer.performOperation("Delete", [lockFiles[i]]);
      }
    } catch (e) {}
  }

  // 3) PowerShell(작은따옴표만 사용)로 잔여 잠금 파일 및 installerResources 폴더 한번 더 정리
  var psSafeDir = resolved.replace(/'/g, "''");
  var psUnlock =
      "Remove-Item -LiteralPath '" + psSafeDir + "/SollunaMailMaintenanceTool.exe'," +
      "'" + psSafeDir + "/SollunaMailMaintenanceTool.dat'," +
      "'" + psSafeDir + "/SollunaMailMaintenanceTool.ini'," +
      "'" + psSafeDir + "/components.xml'," +
      "'" + psSafeDir + "/InstallationLog.txt'," +
      "'" + psSafeDir + "/installer.dat' -Force -ErrorAction SilentlyContinue; " +
      "Remove-Item -LiteralPath '" + psSafeDir + "/installerResources' -Recurse -Force -ErrorAction SilentlyContinue";
  try {
    installer.execute("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", psUnlock]);
  } catch (e) {}
  log("DEBUG: Cleaned IFW lock metadata in " + resolved + ", maintExists=" + installer.fileExists(resolved + "/SollunaMailMaintenanceTool.exe"));
}

// ------------ Controller ctor ------------
function Controller() {
  log("DEBUG: Controller script initialized.");

  findAndSetInstallerPath();
  try {
    installer.setMessageBoxAutomaticAnswer("OverwriteTargetDirectory", QMessageBox["Yes"]);
  } catch (e) {}

  // 시작 상태 로깅
  log("DEBUG: Controller start, isAdmin=" + isAdminNow()
      + ", InstallScope=" + installer.value("InstallScope")
      + ", TargetDir=" + installer.value("TargetDir"));

  // 기본값(비어있을 때만) — 사용자 선택을 덮지 않도록 최소한으로
  if (!installer.value("InstallScope"))
    installer.setValue("InstallScope", "user");

  if (!installer.value("TargetDir")) {
    // Per-User 기본 경로
    installer.setValue("TargetDir", "@HomeDir@/AppData/Local/SOLLUNA_MAIL");
  }
  if (!installer.value("UpgradeMode")) {
    installer.setValue("UpgradeMode", "0");
  }
}

Controller.prototype.TargetDirectoryPageCallback = function () {
  prepareTargetDirForUpgrade(installer.value("TargetDir"));
  var page = gui.pageWidgetByObjectName("TargetDirectoryPage");
  if (page && page.targetDirectoryLineEdit && !page.sollunaUpgradeHooked) {
    page.sollunaUpgradeHooked = true;
    page.targetDirectoryLineEdit.textChanged.connect(function(newText) {
      prepareTargetDirForUpgrade(newText);
    });
  }
};

// ------------ Dynamic pages (Controller 경로 고정) ------------
Controller.prototype.DynamicPerUserMachinePageCallback = function () {
  log("CTRL: DynamicPerUserMachinePageCallback CALLED");

  var page = gui.pageWidgetByObjectName("PerUserMachinePage")
          || gui.pageWidgetByObjectName("DynamicPerUserMachinePage");
  if (!page) { log("CTRL: PerUserMachinePage not found"); return null; }

  page.complete = false;

  // 위젯은 '이름'으로만 찾기 (IFW에서 가장 안정적)
  var perUser    = page.findChild("perUserRadio");
  var perMachine = page.findChild("perMachineRadio");
  var labelHint  = page.findChild("labelHint");

  if (!perUser || !perMachine) {
    log("Z_CRITICAL_ERROR: radios not found on PerUserMachinePage");
    return page;
  }

  if (labelHint) labelHint.visible = !isAdminNow();

  // 초기 반영
  if (installer.value("InstallScope") === "machine") {
    perMachine.checked = true;
    installer.setValue("TargetDir", "@ApplicationsDir@/SOLLUNA_MAIL");
  } else {
    installer.setValue("InstallScope", "user");
    perUser.checked = true;
    installer.setValue("TargetDir", "@HomeDir@/AppData/Local/SOLLUNA_MAIL");
  }
  page.complete = true;
  prepareTargetDirForUpgrade(installer.value("TargetDir"));

  // Per-User 선택
  perUser.toggled.connect(function(on){
    if (!on) return;
    installer.setValue("InstallScope", "user");
    installer.setValue("TargetDir", "@HomeDir@/AppData/Local/SOLLUNA_MAIL");
    page.complete = true;
    prepareTargetDirForUpgrade(installer.value("TargetDir"));

    var t = gui.pageWidgetByObjectName("TargetDirectoryPage");
    if (t && t.targetDirectoryLineEdit)
      t.targetDirectoryLineEdit.setText(installer.value("TargetDir"));
  });

  // Per-Machine 선택
  perMachine.toggled.connect(function(on){
    if (!on) return;
    installer.setValue("InstallScope", "machine");
    installer.setValue("TargetDir", "@ApplicationsDir@/SOLLUNA_MAIL");
    prepareTargetDirForUpgrade(installer.value("TargetDir"));

    if (isAdminNow()) {
      var t = gui.pageWidgetByObjectName("TargetDirectoryPage");
      if (t && t.targetDirectoryLineEdit)
        t.targetDirectoryLineEdit.setText(installer.value("TargetDir"));
      page.complete = true;
      return;
    }

    if (mb_confirm("관리자 권한 필요", "Per-Machine 설치에는 관리자 권한이 필요합니다.\n계속 진행하시겠습니까?")) {
      return;
    }

    // No 또는 실패 → Per-User로 롤백
    perUser.checked = true;
    installer.setValue("InstallScope","user");
    installer.setValue("TargetDir","@HomeDir@/AppData/Local/SOLLUNA_MAIL");
    page.complete = true;

    var t2 = gui.pageWidgetByObjectName("TargetDirectoryPage");
    if (t2 && t2.targetDirectoryLineEdit)
      t2.targetDirectoryLineEdit.setText(installer.value("TargetDir"));
  });

  return page;
};

Controller.prototype.DynamicLaunchOptionPageCallback = function () {
  var page = gui.pageWidgetByObjectName("LaunchOptionPage")
          || gui.pageWidgetByObjectName("DynamicLaunchOptionPage");
  if (!page) return null;

  page.complete = true;
  var cb = page.findChild("launchCheck");
  installer.setValue("LaunchAfterFinish", (cb && cb.checked) ? "1" : "0");
  if (cb) cb.toggled.connect(function(on){
    installer.setValue("LaunchAfterFinish", on ? "1" : "0");
  });
  return page;
};

// ------------ PerformInstallationPage: 아카이브 압축 해제 전 실행 중인 프로세스 강제 종료 ------------
Controller.prototype.PerformInstallationPageCallback = function () {
  try {
    installer.execute("cmd.exe", ["/C", "taskkill /F /IM SollunaMail.exe /T >nul 2>&1 & taskkill /F /IM QtWebEngineProcess.exe /T >nul 2>&1"]);
    log("DEBUG: Terminated running SollunaMail.exe and QtWebEngineProcess.exe before extraction.");
  } catch (e) {
    log("WARN: PerformInstallationPageCallback taskkill error: " + e);
  }
};

// ------------ FinishedPage: 설치 후 실행 ------------
Controller.prototype.FinishedPageCallback = function () {
  var page = gui.pageWidgetByObjectName("FinishedPage");
  if (!page) return null;

  if (installer.value("LaunchAfterFinish") === "1") {
    // 1. "완료" 버튼 (Wizard의 NextButton에 해당)을 가져옵니다.
    var finishButton = gui.button(QInstaller.NextButton);
    
    // 2. 버튼 클릭 시 실행될 함수를 정의합니다.
    var launchApp = function() {
        var exe = installer.value("TargetDir") + "/SollunaMail.exe";
        try {
            // 버튼 클릭 시 프로그램 실행
            installer.executeDetached(exe, [], installer.value("TargetDir"));
            log("DEBUG: Program launched after user clicked Finish.");
        } catch (e) {
            log("WARN: Failed to launch program after installation: " + e);
        }
    };
    
    // 3. 완료 버튼의 clicked 시그널에 launchApp 함수를 연결합니다.
    if (finishButton) {
        finishButton.clicked.connect(launchApp);
        log("DEBUG: Launch action connected to Finish button.");
    }
  }

  return page;
};
