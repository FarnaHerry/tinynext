; tinynext.nsi -- NSIS installer for TinyNext (Windows), Modern UI 2.
; Built by make-win-pkg.ps1 (which first runs make-dist.ps1 to stage ..\dist\).
; Version is injected on the command line:
;     makensis /DAPP_VERSION=x.y.z tinynext.nsi
;
; Requires a FULL NSIS install (Modern UI 2 + SimpChinese language file). The
; choco "nsis" package is a slim build with no Contrib -- CI downloads the full
; NSIS 3.10 zip instead.
;
; IMPORTANT: keep this file ASCII-only (NSIS reads a BOM-less UTF-8 script as
; the ANSI codepage). All Chinese UI text comes from the SimpChinese language
; file, not from strings in this script.

Unicode true

!include "MUI2.nsh"

!ifndef APP_VERSION
  !error "APP_VERSION not defined -- pass /DAPP_VERSION=x.y.z"
!endif

Name "TinyNext ${APP_VERSION}"
OutFile "..\tinynext-v${APP_VERSION}-win64-setup.exe"

; Per-user install (no admin needed). Config/session live in the per-user
; config dir (not cwd), so no "Start in" is required.
InstallDir "$LOCALAPPDATA\Programs\TinyNext"
InstallDirRegKey HKCU "Software\TinyNext" "InstallLocation"

RequestExecutionLevel user
SetCompressor /SOLID lzma

; ---- Modern UI 2 ----
!define MUI_ABORTWARNING
!define MUI_ICON "..\assets\icon.ico"
!define MUI_UNICON "..\assets\icon.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header-r.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\tinynext.exe"

; MUI2 requires MUI_LANGUAGE to come after all MUI_[UN]PAGE_* macros.
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---- install ----
Section "TinyNext" SecMain
    SetOutPath "$INSTDIR"
    File /r "..\dist\*"

    ; Start menu + desktop shortcuts (icon from assets; wizard text is Chinese
    ; via the language file, shortcut names stay ASCII).
    CreateDirectory "$SMPROGRAMS\TinyNext"
    CreateShortCut "$SMPROGRAMS\TinyNext\TinyNext.lnk" "$INSTDIR\tinynext.exe" "" "$INSTDIR\assets\icon.ico" 0 SW_SHOWNORMAL "" "TinyNext"
    CreateShortCut "$SMPROGRAMS\TinyNext\Uninstall TinyNext.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\TinyNext.lnk" "$INSTDIR\tinynext.exe" "" "$INSTDIR\assets\icon.ico" 0 SW_SHOWNORMAL "" "TinyNext"

    ; Uninstaller + Add/Remove Programs entry (per-user, HKCU).
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "DisplayName" "TinyNext ${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "Publisher" "FarnaHerry"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "DisplayIcon" "$INSTDIR\tinynext.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext" "NoRepair" 1
    WriteRegStr HKCU "Software\TinyNext" "InstallLocation" "$INSTDIR"
SectionEnd

; ---- uninstall ----
Section "Uninstall"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir /r "$INSTDIR"
    Delete "$SMPROGRAMS\TinyNext\TinyNext.lnk"
    Delete "$SMPROGRAMS\TinyNext\Uninstall TinyNext.lnk"
    RMDir "$SMPROGRAMS\TinyNext"
    Delete "$DESKTOP\TinyNext.lnk"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TinyNext"
    DeleteRegKey HKCU "Software\TinyNext"
SectionEnd
