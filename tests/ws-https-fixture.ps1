param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$executable = (Resolve-Path $Exe).Path
$openssl = (Get-Command openssl -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$temp = Join-Path ([IO.Path]::GetTempPath()) ("little-wss-" + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temp) | Out-Null

function Invoke-OpenSSL([string[]]$Arguments) {
    $output = & $openssl @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL fixture generation failed: $output" }
}

try {
    $config = Join-Path $temp 'openssl.cnf'
    $configText = @'
[req]
distinguished_name=dn
[dn]
'@
    [IO.File]::WriteAllText($config, $configText)
    $caKey = Join-Path $temp 'ca-key.pem'
    $caCert = Join-Path $temp 'ca.pem'
    $serverKey = Join-Path $temp 'server-key.pem'
    $serverCsr = Join-Path $temp 'server.csr'
    $serverCert = Join-Path $temp 'server.pem'
    $ext = Join-Path $temp 'server.ext'
    Invoke-OpenSSL @('req', '-config', $config, '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', $caKey, '-out', $caCert, '-days', '2', '-subj', '/CN=Little test CA', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    Invoke-OpenSSL @('req', '-config', $config, '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', $serverKey, '-out', $serverCsr, '-subj', '/CN=local-test')
    $extText = "basicConstraints=critical,CA:FALSE" + [char]10 + "keyUsage=critical,digitalSignature,keyEncipherment" + [char]10 + "extendedKeyUsage=serverAuth" + [char]10 + "subjectAltName=IP:127.0.0.1" + [char]10
    [IO.File]::WriteAllText($ext, $extText)
    Invoke-OpenSSL @('x509', '-req', '-in', $serverCsr, '-CA', $caCert, '-CAkey', $caKey, '-set_serial', '1', '-days', '2', '-extfile', $ext, '-out', $serverCert)

    $script = @'
var server = ws.serve(fn(socket, request) {
    socket:next().next(fn(message) {
        socket:send(message)
    }).catch(fn(reason) {
        io.print("server error", reason)
    })
}, {
    hostname: "127.0.0.1"
    port: 0
    tls: { cert: "CERT" key: "KEY" }
})
var port = string.format("%d", server.port)
ws.connect(string.expand("wss://127.0.0.1:{{port}}/secure", { port })).next(fn(socket) {
    io.print("client open")
    socket:send("secure")
    socket:next().next(fn(message) {
        io.print("client got", message)
        socket:close()
        server:close()
        mainloop.stop()
    }).catch(fn(reason) {
        io.print("client error", reason)
        server:close()
        mainloop.stop()
    })
}).catch(fn(reason) {
    io.print(reason)
    server:close()
    mainloop.stop()
})
setTimeout(fn() {
    mainloop.stop()
}, 2000)
mainloop.run()
'@.Replace('CERT', $serverCert.Replace('\','/')).Replace('KEY', $serverKey.Replace('\','/'))
    $fixture = Join-Path $temp 'ws-https.little'
    [IO.File]::WriteAllText($fixture, $script, [Text.UTF8Encoding]::new($false))

    function Run-WSS([string]$Trust, [string]$Expected) {
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = [Diagnostics.ProcessStartInfo]::new($executable)
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.WorkingDirectory = $repo
        $process.StartInfo.Environment['SSL_CERT_FILE'] = $Trust
        $process.StartInfo.ArgumentList.Add($fixture)
        if (-not $process.Start()) { throw 'Could not start Little' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) { $process.Kill(); $process.WaitForExit(); throw 'WSS fixture timed out' }
        $output = $stdout.Result.Replace([string][char]13 + [string][char]10, [string][char]10).TrimEnd()
        $errors = $stderr.Result.Trim()
        if ($process.ExitCode -ne 0 -or $errors -or $output -ne $Expected) {
            $message = "WSS output mismatch (exit $($process.ExitCode)):" + [char]10 + $output + [char]10 + "Errors:" + [char]10 + $errors
            throw $message
        }
        $process.Dispose()
    }

    $trusted = [string]::Join([char]10, @('client open', 'client got secure'))
    Run-WSS $caCert $trusted
    Run-WSS (Join-Path $temp 'missing.pem') 'TLS certificate verification failed'
    Write-Host 'PASS WebSocket HTTPS'
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
