; ---------------------------------------------------------------------------
; Muffin — Windows installer (Inno Setup 6).
;
; Compiled by the `installer` job in .github/workflows/ci.yml:
;   ISCC.exe /DAppVersion=0.2.1 /DStagingDir=C:\...\win\build\Release packaging\windows_installer.iss
; and by hand from a normal Visual Studio build tree (windeployqt must have run first, see README):
;   & "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" packaging\windows_installer.iss
;
; Every path below is anchored to {#SourcePath} — the directory holding this file — so the script
; compiles identically no matter which directory ISCC is invoked from, and the defaults are the
; hand-run case (payload in build\Release, output in build\installer). CI overrides both with
; absolute paths. README.md and LICENSE are read from the repository root, not from StagingDir:
; download-artifact puts them beside build/, not inside build\Release.
;
; The installer deliberately does NOT fetch zapret. Downloading, verifying and installing zapret is
; the GUI's own job (src/core/ZapretDownloader) and it can re-run on demand; duplicating it here
; would put a second network path — with a second, unverified copy of the digest logic — into the
; release.
; ---------------------------------------------------------------------------

#define AppName "Muffin"
#define AppPublisher "F0RSES"
#define AppURL "https://github.com/F0RSES/muffin-gui"
#define RepoDir SourcePath + ".."

; The fallback is for the hand-run case above; CI always passes /DAppVersion from the tag, so this
; default never reaches a published artifact. It still has to equal MUFFIN_VERSION in CMakeLists.txt
; (both mean "the current release line"): a hand-run compile stamps the .exe, its Add/Remove Programs
; entry and VersionInfoVersion from the value read here, so if the two drift apart the version the
; user sees in Windows and the version the GUI reports at runtime disagree. Bump them together.
#ifndef AppVersion
  #define AppVersion "0.2.1"
#endif

; VersionInfoVersion wants a dotted version; a bare tag usually carries three parts, so pad here
; instead of making every caller pass a four-part value.
#ifndef AppVersion4
  #define AppVersion4 AppVersion + ".0"
#endif

#ifndef StagingDir
  #define StagingDir RepoDir + "\build\Release"
#endif

#ifndef OutputDir
  #define OutputDir RepoDir + "\build\installer"
#endif

[Setup]
; AppId is the key Windows uses to decide "this is an upgrade of something already installed", so it
; must never change once a build has shipped: a new value orphans the old entry in "Add or remove
; programs" and installs side by side. Generated once for this project.
AppId={{6CBDE8FE-D38A-4F25-8B2C-B0F651B0A620}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
AppCopyright="Copyright (c) 2026 {#AppPublisher}"
VersionInfoVersion={#AppVersion4}
VersionInfoCompany={#AppPublisher}
VersionInfoProductVersion={#AppVersion4}
VersionInfoDescription={#AppName} Setup

; {autopf} resolves to 64-bit "Program Files" because of ArchitecturesInstallIn64BitMode below.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes

; Admin is a hard requirement, not a preference: zapret's winws/nfqws need elevated rights, and a
; per-user install into {localappdata} would leave the payload writable by the interactive user —
; the wrong thing for a tool that gets elevated to run a packet filter.
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

LicenseFile={#RepoDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; windeployqt lays Qt out as: Qt6*.dll beside the executable, plugins in subdirectories.
; ignoreversion on everything — Muffin has no versioned-file installer logic, and re-running the
; installer over the same directory must always refresh the payload.
Source: "{#StagingDir}\Muffin.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagingDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StagingDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StagingDir}\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs createallsubdirs
; The rest are windeployqt/Qt-version dependent, so they are allowed to be absent: iconengines carries
; qsvgicon.dll, without which every toolbar icon renders blank; tls carries the HTTPS backend the
; GitHub download in ZapretSource needs; networkinformation is QNetworkInfo's plugin.
; skipifsourcedoesntexist keeps a Qt that does not ship one of them from failing the build.
Source: "{#StagingDir}\Qt6*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StagingDir}\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#StagingDir}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#StagingDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#RepoDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; Tasks: desktopicon

[Run]
; Offered but unchecked: right after a fresh install the app has no zapret tree yet, and opening it is
; not a prerequisite for finishing the setup. skipifsilent is the silent-install half of the same
; reasoning: an unattended /SILENT or /VERYSILENT run must not end with a GUI popping up over it.
; (postinstall alone already implies the original, non-elevated user token, which is what the app
; wants -- it escalates itself through PrivilegeHelper when it actually needs to.)
Filename: "{app}\{#AppName}.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall unchecked skipifsilent

