param([string]$vsPath, [string]$outFile)

$batPath = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$tempFile = [System.IO.Path]::GetTempFileName()

cmd /v:on /c "`"$batPath`" -arch=amd64 -host_arch=amd64 -no_logo > nul && set > `"$tempFile`"" 2>$null

if (-not (Test-Path $tempFile)) { exit 1 }
$envLines = Get-Content $tempFile -ErrorAction SilentlyContinue
Remove-Item $tempFile -ErrorAction SilentlyContinue
if ([string]::IsNullOrEmpty($envLines)) { exit 1 }

$writer = [System.IO.StreamWriter]::new($outFile, $false, [System.Text.Encoding]::ASCII)
$vsPaths = @()

foreach ($line in $envLines) {
  if ($line -match '^([A-Za-z]+)=(.*)$') {
    $k = $matches[1].ToUpper()
    $v = $matches[2].Trim()
    switch ($k) {
      'PATH' { $vsPaths = $v -split ';' | Where-Object { $_ -match 'Visual Studio|Windows Kits|VC\\Tools\\MSVC' } }
    }
  }
}

# Convert PATH to Git Bash format with proper quoting for spaces
if ($vsPaths) {
  $convertedPaths = ($vsPaths | ForEach-Object {
    $p = ($_ -replace '\\','/') -replace '^([A-Za-z]):','/$1'
    $p = '/{0}{1}' -f $p.Substring(1,1).ToLower(), $p.Substring(2)
    if ($p.Contains(' ')) { "'$p'" } else { $p }
  }) -join ':'
  $writer.WriteLine("PATH=$convertedPaths")
}

# CL prepend flags for MSVC
$writer.WriteLine("CL='/WX- /wd4311 /wd4312 /wd4267 /wd4244'")

# Windows Kit version
if (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Lib") {
  $kitVersions = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory | Sort-Object Name
  if ($kitVersions) { $writer.WriteLine("WINSDK_VERSION=$($kitVersions[-1].Name)") }
}

$writer.Close()