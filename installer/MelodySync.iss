; ============================================================================
;  MelodySync - Inno Setup 6 打包脚本
;  用法：ISCC.exe installer\MelodySync.iss
;  产物：dist\MelodySync-1.0.0-Setup.exe
;
;  说明：
;   - Qt6 运行库由 windeployqt 预先铺到 dist\MelodySync\（见 README 打包步骤）
;   - yt-dlp.exe / ffmpeg.exe 一并打进安装目录，实现「装完即用」
; ============================================================================

#define MyAppName        "MelodySync"
#define MyAppVersion     "1.0.0"
#define MyAppPublisher   "MelodySync Project"
#define MyAppExeName     "MelodySync.exe"
#define MyBuildDir       "..\build\bin"
#define MyDistDir        "..\dist\MelodySync"

[Setup]
AppId={{9F2C7A14-5B6D-4E38-A1C7-8D3F0B2E6A55}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#MyAppExeName}
MinVersion=10.0

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: unchecked

[Files]
; 主程序 + Qt 运行库 + 第三方 dll（windeployqt 铺好的整个目录）
Source: "{#MyDistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; 下载引擎与转码器
Source: "{#MyBuildDir}\yt-dlp.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#MyBuildDir}\ffmpeg.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
; 可选的 CA 证书包：没有时 libcurl 回落系统证书库，所以允许缺失
Source: "{#MyBuildDir}\cacert.pem"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 仅清理程序目录内的缓存，用户数据（AppData 下的 users.db / settings）保留
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\cache"

[Messages]
chinesesimplified.WelcomeLabel2=即将在您的计算机上安装 [name/ver]。%n%n本软件仅限个人学习与研究使用，不得用于任何商业目的。
