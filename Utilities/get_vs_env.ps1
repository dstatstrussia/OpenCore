param([string]$vsPath, [string]$outFile)

# Run VsDevCmd.bat and capture environment variables
$batPath = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$tempFile = [System.IO.Path]::GetTempFileName()
cmd /c "`"$batPath`" -arch=amd64 -no_logo > nul && set > `"$tempFile`"" 2>$null

if (-not (Test-Path $tempFile)) {
  exit 1
}
$envLines = Get-Content $tempFile -ErrorAction SilentlyContinue
Remove-Item $tempFile -ErrorAction SilentlyContinue

if ([string]::IsNullOrEmpty($envLines)) {
  exit 1
}

# Write output
Set-Content -Path $outFile -Value $null -Encoding ascii

$vsPaths = @()
$includePaths = @()
$libPaths = @()

foreach ($line in $envLines) {
  if ($line -match '^([A-Z]+)=(.*)$') {
    $k = $matches[1]
    $v = $matches[2].Trim()
    switch ($k) {
      'PATH' {
        $vsPaths = $v -split ';' | Where-Object { $_ -match 'Visual Studio|Windows Kits|VC\\Tools\\MSVC|MSVC' }
      }
      'INCLUDE' {
        $includePaths = $v -split ';'
      }
      'LIB' {
        $libPaths = $v -split ';'
      }
    }
  }
}

# Convert PATH to Git Bash format (forward slashes, /c/...)
if ($vsPaths) {
  $convertedPaths = ($vsPaths | ForEach-Object {
    ($_ -replace '\\','/') -replace '^([A-Za-z]):','/$1'
  }) -join ':'
  "PATH=$convertedPaths" | Out-File -FilePath $outFile -Encoding ascii -Append
}

# Keep INCLUDE/LIB as Windows paths (backslashes) for cl.exe
if ($includePaths) {
  $convertedInclude = ($includePaths -join ';')
  "INCLUDE=$convertedInclude" | Out-File -FilePath $outFile -Encoding ascii -Append
}

if ($libPaths) {
  $convertedLib = ($libPaths -join ';')
  "LIB=$convertedLib" | Out-File -FilePath $outFile -Encoding ascii -Append
}