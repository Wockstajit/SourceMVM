; Included by electron-builder ahead of its own NSIS templates (package.json build.nsis.include).
;
; An upgrade runs the previous version's uninstaller, which moves every installed file into
; %TEMP%\nsXXXX.tmp\old-install\ before deleting it. NSIS is not long-path aware, so a file whose
; path passes 260 characters cannot be moved. Builds up to 0.2.0 shipped media with paths that
; long, and upgrading over them failed with "cannot be closed" and then "Failed to uninstall old
; application files: 2". scripts/shorten-bundle-paths.ps1 keeps new builds short; this removes the
; long folder from an existing install, through a \\?\ path, before its uninstaller runs.
;
; customCheckAppRunning runs in the installer right before the old uninstaller, and in the
; uninstaller itself. Defining it replaces electron-builder's check, so declare what that check
; needs and run it first: the app must be closed before its files are touched.

!include "getProcessInfo.nsh"
Var pid

!macro removeLongBundledPaths DIR
  Push $0
  StrCpy $0 "${DIR}"
  ; Only ever inside a real install folder.
  ${if} $0 != ""
  ${andIf} ${FileExists} "$0\${APP_EXECUTABLE_FILENAME}"
  ${andIf} ${FileExists} "$0\resources\bundle\*.*"
    DetailPrint "Removing long bundled paths from $0"
    nsExec::Exec `"$SYSDIR\cmd.exe" /C if exist "\\?\$0\resources\bundle\runtime\osu" rd /s /q "\\?\$0\resources\bundle\runtime\osu"`
    Pop $0
  ${endIf}
  Pop $0
!macroend

!macro customCheckAppRunning
  ; CHECK_APP_RUNNING has already declared and set $CmdPath and $PowerShellPath.
  !insertmacro IS_POWERSHELL_AVAILABLE
  !insertmacro _CHECK_APP_RUNNING

  Push $1
  !insertmacro removeLongBundledPaths "$INSTDIR"
  ReadRegStr $1 HKEY_CURRENT_USER "${INSTALL_REGISTRY_KEY}" InstallLocation
  !insertmacro removeLongBundledPaths "$1"
  ReadRegStr $1 HKEY_LOCAL_MACHINE "${INSTALL_REGISTRY_KEY}" InstallLocation
  !insertmacro removeLongBundledPaths "$1"
  Pop $1
!macroend
