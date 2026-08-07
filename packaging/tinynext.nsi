; tinynext.nsi -- NSIS installer for TinyNext (Windows).
; Built by make-win-pkg.ps1 (which first runs make-dist.ps1 to stage ..\dist\).
; Version is injected on the command line:
;     makensis /DAPP_VERSION=x.y.z tinynext.nsi
;
; IMPORTANT: keep this file ASCII-only. NSIS reads a BOM-less UTF-8 script as
; the ANSI codepage; a non-ASCII byte (e.g. the Chinese shortcut names) would be
; misread on en-US CI runners. The wizard UI is still Chinese because we load
; NSIS's own ChineseSimplified.nlf (encoded properly inside NSIS).

Unicode true

; Chinese wizard UI (Next/Back/Install/Uninstall buttons etc.).
LoadLanguageFile "${NSISDIR}\Contrib\Language files\ChineseSimplified.nlf"

!ifndef APP_VERSION
  !error "APP_VERSION not defined -- pass /DAPP_VERSION=x.y.z"
!endif

Name "TinyNext ${APP_VERSION}"
OutFile "..\tinynext-v${APP_VERSION}-win64-setup.exe"

; Per-user install (no admin needed). TinyNext writes tinynext.conf to its cwd,
; so the shortcut's "Start in" points at the install dir -- config lands next to
; the exe, matching the portable zip behaviour. Program Files would be
; unwritable for the config file.
InstallDir "$LOCALAPPDATA\Programs\TinyNext"
InstallDirRegKey HKCU "Software\TinyNext" "InstallLocation"

RequestExecutionLevel user
SetCompressor /SOLID lzma

Icon "..\assets\icon.ico"

; ---- pages ----
Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

; ---- install ----
Section "TinyNext" SecMain
    SetOutPath "$INSTDIR"
    File /r "..\dist\*"

    ; Start menu + desktop shortcuts. "Start in" = $INSTDIR so the config file
    ; (tinynext.conf) is written next to the exe. Shortcut icon from assets.
    CreateDirectory "$SMPROGRAMS\TinyNext"
    CreateShortCut "$SMPROGRAMS\TinyNext\TinyNext.lnk" "$INSTDIR\tinynext.exe" "" "$INSTDIR\assets\icon.ico" 0 SW_SHOWNORMAL "" "" "TinyNext downloader" "$INSTDIR"
    CreateShortCut "$SMPROGRAMS\TinyNext\Uninstall TinyNext.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\TinyNext.lnk" "$INSTDIR\tinynext.exe" "" "$INSTDIR\assets\icon.ico" 0 SW_SHOWNORMAL "" "" "TinyNext downloader" "$INSTDIR"

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
