param([string]$vsPath, [string]$outFile)

# Run VsDevCmd.bat and capture environment variables  
$joined = cmd /c "`"$vsPath\Common7\Tools\VsDevCmd.bat`"" -arch=amd64 > nul 2>&1 && set

# Parse and write PATH/INCLUDE/LIB with Git Bash path format (C:\ -> /c/)
$joined | ForEach-Object {
  if ($_ -match '^([A-Z]+)=(.*)$' -and $matches[1] -match '^(PATH|INCLUDE|LIB)$') {
    $k = $matches[1]
    $v = $matches[2].Trim()
    # Convert Windows paths to Git Bash paths: C:\... + c:\... -> /c/...
    $v = $v -replace '\\','/'
    $v = $v -replace '([A-Za-z]):','/$1'
    "${k}=${v}" | Out-File -FilePath $outFile -Encoding ascii -Append
  }
}
