param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()

# Use a local port selected by the OS. There is a small bind race after release.
$source = @'
var payload = string.repeat("abc", 4000)
var server = web.serve(fn(request) {
    if request.url is "/inner" {
        return "self-fetch complete"
    }
    if request.url is "/outer" {
        return web.fetch("http://127.0.0.1:PORT/inner").next(fn(inner) {
            return inner.body
        })
    }
    if request.url is "/promise" {
        var response = web.Response("promised", {
            status: 201
            statusText: "Created"
            headers: { "X-Reply": "retained" }
        })
        return Promise(fn(resolve, reject) {
            setTimeout(fn() {
                gc.collect()
                resolve(response)
            }, 1)
        })
    }
    io.print(request.method is "POST")
    io.print(request.url is "/check?x=1")
    io.print(request.headers["X-Test"] is "value")
    io.print(request.body is payload)
    return "response-body"
}, { hostname: "127.0.0.1" port: PORT })
io.print("serve returned")
web.fetch("http://127.0.0.1:PORT/check?x=1", {
    method: "POST"
    headers: { "X-Test": "value" }
    body: payload
}).next(fn(response) {
    io.print(response.status is 200)
    io.print(response.ok)
    io.print(response.body is "response-body")
    response:text().next(fn(text) {
        io.print(text is "response-body")
        web.fetch("http://127.0.0.1:PORT/promise").next(fn(result) {
            io.print(result.status is 201)
            io.print(result.statusText is "Created")
            io.print(result.headers["X-Reply"] is "retained")
            io.print(result.body is "promised")
            gc.collect()
            io.print(result.headers["X-Reply"] is "retained")
            web.fetch("http://127.0.0.1:PORT/outer").next(fn(nested) {
                io.print(nested.body is "self-fetch complete")
                server:close()
            }).catch(fn(reason) {
                io.print(reason)
                server:close()
            })
        }).catch(fn(reason) {
            io.print(reason)
            server:close()
        })
    })
}).catch(fn(reason) {
    io.print(reason)
    server:close()
})
'@.Replace('PORT', $port.ToString())

$fixture = Join-Path $repo 'build/web-loopback.little'
[IO.Directory]::CreateDirectory((Split-Path -Parent $fixture)) | Out-Null
[IO.File]::WriteAllText($fixture, $source, [Text.UTF8Encoding]::new($false))
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = [System.Diagnostics.ProcessStartInfo]::new((Resolve-Path $Exe).Path)
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
$process.StartInfo.WorkingDirectory = $repo
$process.StartInfo.ArgumentList.Add($fixture)
if (-not $process.Start()) { throw 'Could not start Little' }
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
if (-not $process.WaitForExit(15000)) {
    $process.Kill()
    $process.WaitForExit()
    throw 'Web loopback timed out: serve, fetch, or close did not complete'
}
$output = ($stdout.Result -replace "`r`n", "`n").TrimEnd()
$errors = $stderr.Result.Trim()
$expected = (@('serve returned') + @('true') * 14) -join "`n"
if ($process.ExitCode -ne 0 -or $errors -or $output -ne $expected) {
    throw "Web loopback failed (exit $($process.ExitCode)). Output:`n$output`nErrors:`n$errors"
}
Write-Host 'PASS web loopback'
