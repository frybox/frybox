; example2.nsi
;
; This script is based on example1.nsi, but adds uninstall support
; and (optionally) start menu shortcuts.
;
; It will install notepad.exe into a directory that the user selects,
;

; The name of the installer
Name "Frybox"

; The file to write
OutFile "frybox-setup.exe"

; The default installation directory
InstallDir $PROGRAMFILES\Frybox
; Registry key to check for directory (so if you install again, it will
; overwrite the old one automatically)
InstallDirRegKey HKLM SOFTWARE\Frybox "Install_Dir"

; The text to prompt the user to enter a directory
ComponentText "This will install frybox on your computer."
; The text to prompt the user to enter a directory
DirText "Choose a directory to install in to:"

; The stuff to install
Section "Frybox (required)"
  ; Set output path to the installation directory.
  SetOutPath $INSTDIR
  ; Put file there
  File "..\frybox.exe"
  ; Write the installation path into the registry
  WriteRegStr HKLM SOFTWARE\Frybox "Install_Dir" "$INSTDIR"
  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Frybox" "DisplayName" "Frybox (remove only)"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Frybox" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteUninstaller "uninstall.exe"
SectionEnd


; uninstall stuff

UninstallText "This will uninstall frybox. Hit next to continue."

; special uninstall section.
Section "Uninstall"
  ; remove registry keys
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Frybox"
  DeleteRegKey HKLM SOFTWARE\Frybox
  ; remove files
  Delete $INSTDIR\frybox.exe
  ; MUST REMOVE UNINSTALLER, too
  Delete $INSTDIR\uninstall.exe
  ; remove shortcuts, if any.
  RMDir "$SMPROGRAMS\Frybox"
  RMDir "$INSTDIR"
SectionEnd

; eof
