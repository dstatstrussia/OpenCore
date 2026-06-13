param([string]$vsPath, [string]$outFile)

$batPath = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$tempFile = [System.IO.Path]::GetTempFileName()

cmd /v:on /c "`"$batPath`" -arch=amd64 -host_arch=amd64 -no_logo > nul && set > `"$tempFile`"" 2>$null

if (-not (Test-Path $tempFile)) { exit 1 }
$envContent = Get-Content $tempFile -Raw -ErrorAction SilentlyContinue
Remove-Item $tempFile -ErrorAction SilentlyContinue
if ([string]::IsNullOrEmpty($envContent)) { exit 1 }
# Split on any line ending (CRLF or LF) to handle both Windows and Unix
# Use explicit string replacement to handle edge cases
$envContent = $envContent -replace "`r`n", "`n"
$envContent = $envContent -replace "`r", "`n"
$envLines = $envContent -split "`n" | Where-Object { $_ -match '=' }

$writer = [System.IO.StreamWriter]::new($outFile, $false, [System.Text.Encoding]::ASCII)
# Ensure Unix line endings
$writer.NewLine = "`n"
$vsPaths = @()

foreach ($line in $envLines) {
  # Strip any trailing carriage return before matching
  $line = $line.Trim()
  if ($line -match '^([A-Za-z]+)=(.*)$') {
    $k = $matches[1].ToUpper()
    $v = $matches[2].Trim()
    # Store PATH and CL values, but we'll process them separately
    if ($k -eq 'PATH') {
      $vsPaths = $v -split ';' | Where-Object { $_ -match 'Visual Studio|Windows Kits|VC' -and $_ -notmatch 'Git' }
    }
  }
}

# Convert and output PATH in VAR=VALUE format
if ($vsPaths) {
  $convertedPaths = ($vsPaths | ForEach-Object {
    $p = $_.Replace('\','/')
    if ($p -match '^([A-Za-z]):/') {
      $drive = $p.Substring(0,1).ToLower()
      $p = "/$drive" + $p.Substring(3)
    }
    $p
  }) -join ':'
  $writer.WriteLine("PATH=$convertedPaths")
}

# CL flags - suppress warnings for IA32 compatibility builds
# Bash script will handle appending to existing CL if set
$writer.WriteLine("CL=/WX- /wd4311 /wd4312 /wd4267 /wd4244")

if (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Lib") {
  $kitVersions = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory | Sort-Object Name
  if ($kitVersions) { $writer.WriteLine("WINSDK_VERSION=$($kitVersions[-1].Name)") }
}

$writer.Close()