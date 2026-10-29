param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$executable = (Resolve-Path $Exe).Path
$crlf = [string][char]13 + [string][char]10
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$process = $null
$client = $null
try {
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $script = @'
io.print("start")
ws.connect("ws://127.0.0.1:PORT/eof").next(fn(socket) {
    socket:next().catch(fn(reason) {
        io.print("closed", socket.closed)
        io.print("error", string.contains(reason, "WebSocket receive failed"))
        mainloop.stop()
    })
}).catch(fn(reason) {
    io.print("connect error", reason)
    mainloop.stop()
})
setTimeout(fn() {
    mainloop.stop()
}, 1500)
mainloop.run()
io.print("done")
'@
    $fixture = Join-Path $repo 'build/ws-eof.little'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $fixture)) | Out-Null
    [IO.File]::WriteAllText($fixture, $script.Replace('PORT', $port.ToString()), [Text.UTF8Encoding]::new($false))
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
    if (-not $accept.Wait(10000)) { throw 'Timed out waiting for WebSocket connection' }
    $client = $accept.Result
    $stream = $client.GetStream()
    $stream.ReadTimeout = 5000
    $buffer = [byte[]]::new(4096)
    $request = [Text.StringBuilder]::new()
    $terminator = $crlf + $crlf
    while (-not $request.ToString().Contains($terminator)) {
        $count = $stream.Read($buffer, 0, $buffer.Length)
        if ($count -eq 0) { throw 'Client closed before sending headers' }
        [void]$request.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $count))
    }
    $match = [regex]::Match($request.ToString(), 'Sec-WebSocket-Key:\s*([^\r\n]+)')
    if (-not $match.Success) { throw 'Missing Sec-WebSocket-Key' }
    $key = $match.Groups[1].Value.Trim()
    $sha = [Security.Cryptography.SHA1]::Create()
    $digest = $sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'))
    $acceptKey = [Convert]::ToBase64String($digest)
    $headers = 'HTTP/1.1 101 Switching Protocols' + $crlf + 'Upgrade: websocket' + $crlf + 'Connection: Upgrade' + $crlf + 'Sec-WebSocket-Accept: ' + $acceptKey + $crlf + $crlf
    $headerBytes = [Text.Encoding]::ASCII.GetBytes($headers)
    $stream.Write($headerBytes, 0, $headerBytes.Length)
    $stream.Flush()
    $client.Close()
    $client = $null

    if (-not $process.WaitForExit(10000)) { throw 'Timed out waiting for Little to exit' }
    $output = $stdout.Result.Replace($crlf, [string][char]10).TrimEnd()
    $errors = $stderr.Result.Trim()
    $expected = [string]::Join([char]10, @('start', 'closed true', 'error true', 'done'))
    if ($process.ExitCode -ne 0 -or $errors -or $output -ne $expected) {
        throw ('EOF fixture failed (exit ' + $process.ExitCode + '):' + [char]10 + $output + [char]10 + $errors)
    }
    Write-Host 'PASS WebSocket EOF'
}
finally {
    if ($client) { $client.Close() }
    $listener.Stop()
    if ($process) {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
