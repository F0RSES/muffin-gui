; ---------------------------------------------------------------------------
; Muffin — Windows installer (Inno Setup 6).
;
; Compiled by the `installer` job in .github/workflows/ci.yml:
;   ISCC.exe /DAppVersion=0.4.1 /DStagingDir=C:\...\win\build\Release packaging\windows_installer.iss
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
  #define AppVersion "0.5.0"
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

; The wizard's look. `modern` is a base style (white page content, and the bevel between the top
; panel and the page content is hidden) and `dark` is an appearance mode layered on it, which is why
; they are two space-separated words in one value: the WizardStyle topic gives its valid values as
; "one or more of the following, separated by spaces: classic modern light dark dynamic polar slate
; stellar windows11 zircon ...", and its own examples include `modern dark`.
;
; Appearance modes are new in Inno Setup 6.6; an older compiler rejects the value outright. The
; `installer` job therefore probes the compiler for this exact directive before building the real
; script -- reading ISCC's version resource would not work, because jrsoftware ships 6.7.3 with
; FileVersion and ProductVersion both stamped 0.0.0.0 -- and it also looks for Inno Setup 7, which
; installs beside 6 rather than over it.
;
; `dark` is not decorative here: the application's own sheet is a dark one (#0B0C10 canvas, neon
; accents), and a bright grey wizard was the one place in the release where the product did not look
; like the product. Forced dark also decides which image directive is read: WizardImageFileDynamicDark
; is documented as overriding WizardImageFile "when using WizardStyle with dynamic mode and Windows is
; in dark mode. Ignored otherwise", so with a forced mode the plain names below are what take effect
; and only one image each is needed.
WizardStyle=modern dark

; The two banners, both rendered by packaging/make-wizard-images.py from the same coordinates as
; src/resources/icons/app.svg, so the installer shows the application's mark rather than a picture of
; it. The sizes are that script's BANNER_SIZE and SMALL_SIZE, chosen from the [Setup] reference: the
; tall image's area keeps a 164:314 aspect ratio and measures 202x386 at 100% DPI, with "an image
; larger than 202x386 ... recommended to avoid the image looking blurry on higher-DPI systems" and
; Inno's built-in art at 240x459 -- so 240x459. The small image's area is square, 58x58 at 100% DPI,
; and Inno's built-in is 147x147, which is what is shipped here.
;
; BMP because that is what the directives take; .png is accepted too but a BMP has no decoder to
; agree with, and these files are checked as bytes by packaging/check-win-resources.py, which also
; regenerates them and fails if the committed bytes are not what the script produces. Both paths are
; anchored to {#SourcePath} like every other file here: ISCC resolves a relative WizardImageFile
; against the directory of the script, and a missing file is a compile error -- which, with the
; `installer` job gated on a tag, would otherwise be the first anyone ever learned.
WizardImageFile={#SourcePath}wizard_image.bmp
WizardSmallImageFile={#SourcePath}wizard_small.bmp
; The colour used to fill any space around the image that the bitmap does not cover. Both images match
; their areas closely but not exactly (240:459 against an area of 202:386), and the tall one is a
; fraction off, so a sliver can show at some DPI settings. Inno's own default for a forced dark mode
; is #2e3a3f -- already dark, not the light halo it would have been under the old light default -- but
; the application's canvas is #0B0C10, so that is what both fills are set to: the banner should be
; unable to disagree with the window it is standing in for. WizardImageBackColor covers the tall image
; and WizardSmallImageBackColor the square one; they are separate directives.
WizardImageBackColor=#0B0C10
WizardSmallImageBackColor=#0B0C10
; Stretched or shrunk to fit, which is Inno's default (WizardImageStretch=yes) and is stated so that
; the reason the banners can be one fixed size is written down next to them.
WizardImageStretch=yes

UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}

; The setup program's own icon. Without this the .exe everyone downloads is Inno's
; default blue-arrow wizard icon, which is the one place the brand was missing even
; before the application itself had one. Same .ico app.rc compiles into Muffin.exe, so
; there is one artwork file to keep in sync, not two.
SetupIconFile={#RepoDir}\packaging\windows\muffin.ico

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
; The icon, installed beside the executable. The shortcuts below do not need it — they
; bind to Muffin.exe, which carries the same image compiled in by packaging/windows/app.rc —
; but a user who pins the app or builds a shortcut from Explorer resolves the icon from a
; file, and this is the copy the .iss and the .rc both point at.
Source: "{#RepoDir}\packaging\windows\muffin.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; The icon these shortcuts show is the icon group embedded in Muffin.exe by
; packaging/windows/app.rc — that is what fixes the blank Start-menu / desktop / taskbar
; icon, and no .ico file has to travel with the shortcut for it to work. IconFilename is
; still spelled out rather than left to the implicit "use the target's icon" fallback: the
; fallback is exactly what rendered blank while the executable carried no icon group, and
; naming it pins the shortcut to entry 0 of the exe instead of to a loose file in {app}.
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; IconFilename: "{app}\{#AppName}.exe"; IconIndex: 0
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; IconFilename: "{app}\{#AppName}.exe"; IconIndex: 0; Tasks: desktopicon

[Run]
; Offered but unchecked: right after a fresh install the app has no zapret tree yet, and opening it is
; not a prerequisite for finishing the setup. skipifsilent is the silent-install half of the same
; reasoning: an unattended /SILENT or /VERYSILENT run must not end with a GUI popping up over it.
; postinstall starts the program with the original, non-elevated user token, and since 0.4.1
; Muffin.exe's own manifest asks Windows for requireAdministrator -- so ticking this box costs the user
; one UAC consent. That is the intended shape of it: the elevation belongs to the application (see
; packaging/windows/app.manifest), and launching it from here with runascurrentuser would only hide
; that same prompt behind the setup's own consent, with the app inheriting the installer's token.
Filename: "{app}\{#AppName}.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall unchecked skipifsilent

