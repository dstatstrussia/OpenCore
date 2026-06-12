param([string]$vsPath, [string]$outFile)

# Run VsDevCmd.bat and capture ALL environment variables after it runs
$batPath = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$tempFile = [System.IO.Path]::GetTempFileName()
cmd /c "`"$batPath`" -arch=amd64 -no_logo && set > `"$tempFile`"" 2>$null
if (-not (Test-Path $tempFile)) {
  exit 1
}
$envOutput = Get-Content $tempFile
Remove-Item $tempFile -ErrorAction SilentlyContinue

# Parse and write PATH/INCLUDE/LIB with Git Bash path format
Set-Content -Path $outFile -Value $null -Encoding ascii

$vsPaths = @()
$includePaths = @()
$libPaths = @()

$envOutput | ForEach-Object {
  if ($_ -match '^([A-Z]+)=(.*)$') {
    $k = $matches[1]
    $v = $matches[2].Trim()
    switch ($k) {
      'PATH' {
        $parts = $v -split ';' | Where-Object { $_ -match 'Visual Studio|Windows Kits|VC\\Tools\\MSVC|MSVC\\' }
        $vsPaths = $parts
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

function Convert-ToGitBashPath([string[]]$paths) {
  ($paths | ForEach-Object {
    ($_ -replace '\\','/') -replace '^([A-Za-z]):','/$1'
  }) -join ':'
}

function Convert-ToGitBashPathSemicolon([string[]]$paths) {
  ($paths | ForEach-Object {
    ($_ -replace '\\','/') -replace '^([A-Za-z]):','/$1'
  }) -join ';'
}

if ($vsPaths) {
  $converted = Convert-ToGitBashPath $vsPaths
  "PATH=$converted" | Out-File -FilePath $outFile -Encoding ascii -Append
}
if ($includePaths) {
  $converted = Convert-ToGitBashPathSemicolon $includePaths
  "INCLUDE=$converted" | Out-File -FilePath $outFile -Encoding ascii -Append
}
if ($libPaths) {
  $converted = Convert-ToGitBashPathSemicolon $libPaths
  "LIB=$converted" | Out-File -FilePath $outFile -Encoding ascii -Append
}