;
; Copyright (c) 2024 Ju Zejian
;
; This program is free software; you can redistribute it and/or
; modify it under the terms of the Simplified BSD License (also
; known as the "2-Clause License" or "FreeBSD License".)
;
; This program is distributed in the hope that it will be useful,
; but without any warranty; without even the implied warranty of
; merchantability or fitness for a particular purpose.
;
; Author contact information:
;   frybox123@gmail.com
;   http://www.frybox.org
;

[Setup]
ArchitecturesAllowed=x86 x64
AlwaysShowComponentsList=false
AppCopyright=Copyright (c) Ju Zejian  All rights reserved.
AppID={{f1c25a1f-3954-4e1a-ac36-4314c52f057c}
AppName=Frybox
AppPublisher=Frybox Development Team
AppPublisherURL=https://frybox.org/
AppSupportURL=https://frybox.org/
AppUpdatesURL=https://frybox.org/
AppVerName=Fossil v{#AppVersion}
AppVersion={#AppVersion}
AppComments=Simple, high-reliability, distributed software configuration management system.
AppReadmeFile=https://frybox.org/home/doc/tip/www/quickstart.wiki
DefaultDirName={pf}\Frybox
DefaultGroupName=Frybox
OutputBaseFilename=frybox-win32-{#AppVersion}
OutputManifestFile=frybox-win32-{#AppVersion}-manifest.txt
SetupLogging=true
UninstallFilesDir={app}\uninstall
VersionInfoVersion={#AppVersion}

[Components]
Name: Application; Description: Core application.; Types: custom compact full; Flags: fixed

[Dirs]
Name: {app}\bin

[Files]
Components: Application; Source: ..\frybox.exe; DestDir: {app}\bin; Flags: restartreplace uninsrestartdelete

[Registry]
Components: Application; Root: HKLM32; SubKey: Software\Frybox; ValueType: string; ValueName: Install_Dir; ValueData: {app}; Flags: uninsdeletekeyifempty uninsdeletevalue
