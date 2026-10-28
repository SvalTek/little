param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$executable = (Resolve-Path $Exe).Path

function Test-FetchFixture([string]$Name, [string]$Script, [string]$Expected, [string]$Response) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $process = $null
    $client = $null
    try {
        $listener.Start()
        $port = $listener.LocalEndpoint.Port
        $fixture = Join-Path $repo "build/web-fetch-$Name.little"
        [IO.Directory]::CreateDirectory((Split-Path -Parent $fixture)) | Out-Null
        [IO.File]::WriteAllText($fixture, $Script.Replace('PORT', $port.ToString()), [Text.UTF8Encoding]::new($false))

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = [Diagnostics.ProcessStartInfo]::new($executable)
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.WorkingDirectory = $repo
        $process.StartInfo.ArgumentList.Add($fixture)
        if (-not $process.Start()) { throw 'Could not start Little' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()

        $accept = $listener.AcceptTcpClientAsync()
        if (-not $accept.Wait(10000)) { throw "Timed out waiting for $Name request" }
        $client = $accept.Result
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $request = [Text.StringBuilder]::new()
        $buffer = [byte[]]::new(4096)
        while (-not $request.ToString().Contains("`r`n`r`n")) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if ($count -eq 0) { throw 'Client closed before sending request headers' }
            [void]$request.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $count))
            if ($request.Length -gt 65536) { throw 'Request headers exceed fixture limit' }
        }
        if (-not $request.ToString().StartsWith("GET /$Name HTTP/1.")) {
            throw "Unexpected request: $request"
        }
        if ($Response) {
            $bytes = [Text.Encoding]::ASCII.GetBytes($Response)
            $stream.Write($bytes, 0, $bytes.Length)
        }
        $client.Close()
        $client = $null

        if (-not $process.WaitForExit(10000)) { throw 'Timed out waiting for Little to exit' }
        $output = ($stdout.Result -replace "`r`n", "`n").TrimEnd()
        $errors = $stderr.Result.Trim()
        if ($process.ExitCode -ne 0 -or $errors -or $output -ne $Expected) {
            throw "Exit $($process.ExitCode). Output:`n$output`nErrors:`n$errors"
        }
        Write-Host "PASS fetch $Name"
    }
    finally {
        if ($client) { $client.Close() }
        $listener.Stop()
        if ($process) {
            if (-not $process.HasExited) {
                $process.Kill()
                $process.WaitForExit()
            }
            $process.Dispose()
        }
    }
}

$chunkedScript = @'
web.fetch("http://127.0.0.1:PORT/chunked").next(fn(response) {
    io.print(response.status is 200)
    io.print(response.body is "Wikipedia")
    response:text().next(fn(text) { io.print(text is "Wikipedia") })
}).catch(fn(reason) { io.print("chunked response rejected") })
'@
$emptyScript = @'
web.fetch("http://127.0.0.1:PORT/empty").next(fn(response) {
    io.print("empty response fulfilled")
}).catch(fn(reason) {
    io.print("empty response rejected")
})
'@
$response = "HTTP/1.1 200 OK`r`nTransfer-Encoding: chunked`r`nConnection: close`r`n`r`n4`r`nWiki`r`n5`r`npedia`r`n0`r`n`r`n"
$failures = @()
try { Test-FetchFixture 'chunked' $chunkedScript (@('true', 'true', 'true') -join "`n") $response }
catch { $failures += "chunked: $_" }
try { Test-FetchFixture 'empty' $emptyScript 'empty response rejected' '' }
catch { $failures += "empty: $_" }
if ($failures.Count) { throw ($failures -join "`n") }
