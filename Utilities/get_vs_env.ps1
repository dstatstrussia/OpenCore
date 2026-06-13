param([string]$vsPath, [string]$outFile)

# Run VsDevCmd.bat and capture environment variables
$batPath = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$tempFile = [System.IO.Path]::GetTempFileName()

# Use cmd /c to run VsDevCmd.bat and capture environment
cmd /v:on /c "`"$batPath`" -arch=amd64 -host_arch=amd64 -no_logo > nul && set > `"$tempFile`"" 2>$null

if (-not (Test-Path $tempFile)) {
  exit 1
}
$envLines = Get-Content $tempFile -ErrorAction SilentlyContinue
Remove-Item $tempFile -ErrorAction SilentlyContinue

if ([string]::IsNullOrEmpty($envLines)) {
  exit 1
}

# Write output with Unix line endings for bash parsing
$writer = [System.IO.StreamWriter]::new($outFile, $false, [System.Text.Encoding]::ASCII)

$vsPaths = @()
$includePaths = @()
$libPaths = @()

foreach ($line in $envLines) {
  # Case-insensitive match for environment variables
  if ($line -match '^([A-Za-z]+)=(.*)$') {
    $k = $matches[1].ToUpper()
    $v = $matches[2].Trim()
    switch ($k) {
      'PATH' {
        $vsPaths = $v -split ';' | Where-Object { $_ -match 'Visual Studio|Windows Kits|VC\\Tools\\MSVC' }
      }
      'INCLUDE' {
        $includePaths = $v -split ';' | Where-Object { $_ -notmatch 'IA32|Ia32|Win32|i386' }
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
  $writer.WriteLine("PATH=$convertedPaths")
}

# Keep INCLUDE/LIB as Windows paths (double backslashes for Git Bash escaping)
if ($includePaths) {
  $convertedInclude = ($includePaths | ForEach-Object { ($_ -replace '\\','\\') }) -join ';'
  $writer.WriteLine("INCLUDE=$convertedInclude")
}

if ($libPaths) {
  $convertedLib = ($libPaths | ForEach-Object { ($_ -replace '\\','\\') }) -join ';'
  $writer.WriteLine("LIB=$convertedLib")
}

# Disable warnings as errors and C4311/C4312/C4267/C4244 for IA32 header compatibility in host tools
# Note: _CL_ appends flags after command line instead of prepending
# Use backslash-space to preserve the entire value as a single token in bash
$writer.WriteLine("_CL_=/WX-\ /wd4311\ /wd4312\ /wd4267\ /wd4244")

# Also output the Windows Kit version for dynamic paths
if (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Lib") {
  $kitVersions = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory | Sort-Object Name
  if ($kitVersions) {
    $writer.WriteLine("WINSDK_VERSION=$($kitVersions[-1].Name)")
  }
}

$writer.Close()
