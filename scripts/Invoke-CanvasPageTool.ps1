# Calls the Workbench's registered page tools in the existing embedded canvas.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CanvasOrigin,
    [ValidateSet('Current', 'Call', 'Result')][string]$Mode = 'Call',
    [ValidatePattern('^(launcher|prj_[A-Za-z0-9_-]{1,96})$')][string]$ProjectId,
    [ValidatePattern('^canvas_[a-z_]+$')][string]$Tool,
    [hashtable]$Arguments = @{},
    [ValidatePattern('^[A-Za-z0-9_-]{1,100}$')][string]$OpId,
    [ValidateRange(1, 120)][int]$TimeoutSec = 30,
    [ValidatePattern('^[A-Za-z0-9_-]{1,100}$')][string]$ExpectedSessionId,
    [ValidatePattern('^[A-Za-z0-9_-]{1,100}$')][string]$ExpectedEpoch
)

$ErrorActionPreference = 'Stop'
if ($CanvasOrigin -cnotmatch '^http://(127\.0\.0\.1|localhost):([1-9][0-9]{0,4})/?$' -or [int]$Matches[2] -gt 65535) {
    throw 'Use the same explicit loopback HTTP origin shown in the UE panel.'
}
$baseUri = $CanvasOrigin.TrimEnd('/')
if ($Mode -eq 'Call' -and (!$ProjectId -or !$Tool)) { throw 'Call requires ProjectId and Tool.' }
if ($Mode -eq 'Result' -and !$OpId) { throw 'Result requires OpId.' }
if (($ExpectedSessionId -or $ExpectedEpoch) -and (!$ExpectedSessionId -or !$ExpectedEpoch -or !$ProjectId -or $Mode -ne 'Call')) {
    throw 'ExpectedSessionId and ExpectedEpoch must be used together in Call mode with ProjectId.'
}

# Let the regular local HTTP session handle its own cookie. No browser page,
# credential file, or global Workbench setting is needed.
$transport = @{ NoProxy = $true; MaximumRedirection = 0; TimeoutSec = 5 }
$null = Invoke-WebRequest @transport -Uri "$baseUri/" -SessionVariable canvasHttpSession
$transport.WebSession = $canvasHttpSession

function Invoke-PageBridge {
    param([string]$Path, [hashtable]$Payload)
    $request = @{ Uri = "$baseUri/api/agent/canvas/$Path"; SkipHttpErrorCheck = $true }
    if ($PSBoundParameters.ContainsKey('Payload')) {
        $request.Method = 'Post'
        $request.ContentType = 'application/json; charset=utf-8'
        $request.Body = $Payload | ConvertTo-Json -Depth 30 -Compress
    }
    $response = Invoke-WebRequest @transport @request
    $reply = $response.Content | ConvertFrom-Json
    if ($response.StatusCode -ge 400) {
        throw "Canvas page bridge returned HTTP $($response.StatusCode) [$($reply.code)] $($reply.message)"
    }
    return $reply
}

if ($Mode -eq 'Result') { return Invoke-PageBridge "commands/$OpId" }
$lookup = if ($ProjectId) { "current?projectId=$ProjectId" } else { 'current' }
$page = Invoke-PageBridge $lookup
if ($Mode -eq 'Current') { return $page }
if ($ExpectedSessionId -and ($page.sessionId -cne $ExpectedSessionId -or $page.epoch -cne $ExpectedEpoch -or $page.projectId -cne $ProjectId)) {
    throw 'The canvas page or Workbench session changed. No command was submitted. Check earlier operations with Result before capturing a new page.'
}

if (!$OpId) { $OpId = [Guid]::NewGuid().ToString('N') }
$command = @{
    epoch = $page.epoch
    sessionId = $page.sessionId
    projectId = $ProjectId
    opId = $OpId
    tool = $Tool
    args = $Arguments
}
try {
    $receipt = Invoke-PageBridge 'commands' $command
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($receipt.status -in @('queued', 'dispatched') -and $timer.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Start-Sleep -Milliseconds 400
        $receipt = Invoke-PageBridge "commands/$OpId"
    }
} catch {
    throw "Command $OpId was not confirmed. Use Result with the same OpId to check its status. $($_.Exception.Message)"
}
if ($receipt.status -ne 'done') { throw "Command $OpId ended in state '$($receipt.status)'. Check it with Result before retrying." }
if ($receipt.result.ok -ne $true) { throw "Page tool failed for command $OpId. $($receipt.result.error)" }
return $receipt
