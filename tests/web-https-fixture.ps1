param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$executable = (Resolve-Path $Exe).Path
$openssl = (Get-Command openssl -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$temp = Join-Path ([IO.Path]::GetTempPath()) ("little-https-" + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temp) | Out-Null

function Invoke-OpenSSL([string[]]$Arguments) {
    # Certificate generation is the only use of the OpenSSL executable.
    $output = & $openssl @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL fixture generation failed: $output" }
}

function New-ServerCertificate([string]$Name, [string]$Address) {
    $key = Join-Path $temp "$Name-key.pem"
    $csr = Join-Path $temp "$Name.csr"
    $cert = Join-Path $temp "$Name.pem"
    $pfx = Join-Path $temp "$Name.pfx"
    $ext = Join-Path $temp "$Name.ext"
    [IO.File]::WriteAllText($ext, "basicConstraints=critical,CA:FALSE`nkeyUsage=critical,digitalSignature,keyEncipherment`nextendedKeyUsage=serverAuth`nsubjectAltName=IP:$Address`n")
    Invoke-OpenSSL @('req', '-config', $config, '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', $key, '-out', $csr, '-subj', '/CN=local-test')
    Invoke-OpenSSL @('x509', '-req', '-in', $csr, '-CA', $caCert, '-CAkey', $caKey, '-set_serial', '1', '-days', '2', '-extfile', $ext, '-out', $cert)
    Invoke-OpenSSL @('pkcs12', '-export', '-out', $pfx, '-inkey', $key, '-in', $cert, '-passout', 'pass:fixture')
    return [Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, 'fixture')
}

function Test-Https([string]$Name, $Certificate, [string]$TrustFile, [bool]$Success, [switch]$NativeTrust) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $client = $null
    $tls = $null
    $process = $null
    try {
        $listener.Start()
        $port = $listener.LocalEndpoint.Port
        $script = @'
web.fetch("https://127.0.0.1:PORT/check").next(fn(response) {
    io.print(response.status is 200)
    io.print(response.body is "secure")
}).catch(fn(reason) {
    io.print(reason)
})
'@
        $fixture = Join-Path $temp "$Name.little"
        [IO.File]::WriteAllText($fixture, $script.Replace('PORT', $port.ToString()), [Text.UTF8Encoding]::new($false))
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = [Diagnostics.ProcessStartInfo]::new($executable)
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.WorkingDirectory = $repo
        if ($NativeTrust) {
            [void]$process.StartInfo.Environment.Remove('SSL_CERT_FILE')
            [void]$process.StartInfo.Environment.Remove('SSL_CERT_DIR')
        }
        else {
            $process.StartInfo.Environment['SSL_CERT_FILE'] = $TrustFile
            $process.StartInfo.Environment['SSL_CERT_DIR'] = $emptyTrustDir
        }
        $process.StartInfo.ArgumentList.Add($fixture)
        if (-not $process.Start()) { throw 'Could not start Little' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()

        $accept = $listener.AcceptTcpClientAsync()
        if (-not $accept.Wait(10000)) { throw "Timed out waiting for $Name connection" }
        $client = $accept.Result
        $tls = [Net.Security.SslStream]::new($client.GetStream(), $false)
        $handshake = $tls.AuthenticateAsServerAsync($Certificate, $false, [Security.Authentication.SslProtocols]::None, $false)
        try {
            if (-not $handshake.Wait(10000)) { throw 'TLS handshake timed out' }
            if ($Success) {
                $buffer = [byte[]]::new(4096)
                $request = [Text.StringBuilder]::new()
                while (-not $request.ToString().Contains("`r`n`r`n")) {
                    $read = $tls.ReadAsync($buffer, 0, $buffer.Length)
                    if (-not $read.Wait(10000)) { throw 'TLS request timed out' }
                    if ($read.Result -eq 0) { throw 'Client closed before request headers' }
                    [void]$request.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $read.Result))
                    if ($request.Length -gt 65536) { throw 'Request headers exceed fixture limit' }
                }
                if (-not $request.ToString().StartsWith('GET /check HTTP/1.')) { throw "Unexpected request: $request" }
                $response = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: 6`r`nConnection: close`r`n`r`nsecure")
                $tls.Write($response, 0, $response.Length)
                $tls.Flush()
            }
        }
        catch {
            if ($Success) { throw }
            # A rejecting client may abort the handshake; its output is checked below.
        }

        if (-not $process.WaitForExit(15000)) { throw "Timed out waiting for $Name result" }
        $output = ($stdout.Result -replace "`r`n", "`n").TrimEnd()
        $errors = $stderr.Result.Trim()
        $expected = if ($Success) { "true`ntrue" } else { 'TLS certificate verification failed' }
        if ($process.ExitCode -ne 0 -or $errors -or $output -ne $expected) {
            throw "$Name failed (exit $($process.ExitCode)). Output:`n$output`nErrors:`n$errors"
        }
        Write-Host "PASS HTTPS $Name"
    }
    finally {
        if ($tls) { $tls.Dispose() }
        if ($client) { $client.Close() }
        $listener.Stop()
        if ($process) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
}

$matching = $null
$mismatched = $null
try {
    $caKey = Join-Path $temp 'ca-key.pem'
    $caCert = Join-Path $temp 'ca.pem'
    $otherKey = Join-Path $temp 'other-key.pem'
    $otherCert = Join-Path $temp 'other.pem'
    $config = Join-Path $temp 'openssl.cnf'
    [IO.File]::WriteAllText($config, "[req]`ndistinguished_name=dn`n[dn]`n")
    $emptyTrustDir = Join-Path $temp 'empty-trust'
    [IO.Directory]::CreateDirectory($emptyTrustDir) | Out-Null
    foreach ($authority in @(@($caKey, $caCert), @($otherKey, $otherCert))) {
        Invoke-OpenSSL @('req', '-config', $config, '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', $authority[0], '-out', $authority[1], '-days', '2', '-subj', '/CN=Little test CA', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    }
    $matching = New-ServerCertificate 'matching' '127.0.0.1'
    $mismatched = New-ServerCertificate 'mismatched' '127.0.0.2'
    Test-Https 'trusted' $matching $caCert $true
    Test-Https 'untrusted' $matching $otherCert $false
    Test-Https 'hostname-mismatch' $mismatched $caCert $false
    if ($env:OS -eq 'Windows_NT') {
        Test-Https 'windows-native-untrusted' $matching '' $false -NativeTrust
    }
}
finally {
    if ($matching) { $matching.Dispose() }
    if ($mismatched) { $mismatched.Dispose() }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
