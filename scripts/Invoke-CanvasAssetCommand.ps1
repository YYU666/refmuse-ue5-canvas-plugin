# Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
# Requires PowerShell 7. The editor logs its opt-in, per-process inbox.
param(
    [Parameter(Mandatory)][string]$Inbox,
    [Parameter(Mandatory)][ValidateSet('Copy','Paste','Result')][string]$Action,
    [string]$ProjectId,
    [string]$NodeId,
    [string]$CopyId,
    [string]$Destination,
    [string]$RequestId,
    [ValidateRange(1,300)][int]$TimeoutSeconds = 40
)
$ErrorActionPreference = 'Stop'
$directory = Get-Item -LiteralPath $Inbox
if (!$directory.PSIsContainer -or $directory.Name -cnotmatch '\A[0-9a-f]{32}\z' -or
    $directory.Parent.Name -cne 'AssetCommands' -or $directory.Parent.Parent.Name -cne 'RefmuseCanvas') {
    throw 'Use the Asset command inbox path from the editor log.'
}
for ($folder = $directory; $folder; $folder = $folder.Parent) {
    if ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'A redirected asset command inbox is invalid.'
    }
}
if (!$RequestId -and $Action -ne 'Result') { $RequestId = [guid]::NewGuid().ToString('N') }
if ($RequestId -cnotmatch '\A[0-9a-f]{32}\z' -or $RequestId -eq ('0' * 32)) {
    throw 'Use a nonzero request ID of 32 lowercase hex digits.'
}
$requestPath = Join-Path $directory.FullName "$RequestId.request.json"
$resultPath = Join-Path $directory.FullName "$RequestId.result.json"

if ($Action -ne 'Result') {
    if ($ProjectId -cnotmatch '\Aprj_[a-z0-9_-]{1,72}\z') { throw 'A canvas project ID is required.' }
    $payload = @{ action = $Action.ToLowerInvariant(); projectId = $ProjectId }
    if ($Action -eq 'Copy') {
        if ($NodeId -cnotmatch '\A[A-Za-z0-9_-]{1,128}\z') { throw 'A saved canvas node ID is required.' }
        $payload.nodeId = $NodeId
    } else {
        if ($CopyId -cnotmatch '\A[0-9a-f]{32}\z' -or !$Destination -or $Destination.Length -gt 1024) {
            throw 'Use copyId from the copy receipt and a destination package folder under /Game.'
        }
        $payload.copyId = $CopyId
        $payload.destination = $Destination
    }
    if (Test-Path -LiteralPath $resultPath) { throw "Request $RequestId already has a result. Use -Action Result." }
    $temporary = Join-Path $directory.FullName "$([guid]::NewGuid().ToString('N')).tmp"
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Compress))
        if ($bytes.Length -gt 4096) { throw 'The asset command is too large.' }
        $file = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew)
        try { $file.Write($bytes, 0, $bytes.Length) } finally { $file.Dispose() }
        [IO.File]::Move($temporary, $requestPath)
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    if ([IO.File]::Exists($resultPath)) {
        $resultFile = Get-Item -LiteralPath $resultPath
        if (($resultFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $resultFile.Length -gt 16384) {
            throw "Invalid receipt for $RequestId."
        }
        $result = Get-Content -LiteralPath $resultPath -Raw -Encoding utf8 | ConvertFrom-Json
        if ($result.requestId -cne $RequestId -or $result.ok -isnot [bool]) {
            throw "Invalid receipt for $RequestId."
        }
        return $result
    }
    Start-Sleep -Milliseconds 100
}
throw "No receipt received for $RequestId. Query -Action Result -RequestId $RequestId before issuing another command."
