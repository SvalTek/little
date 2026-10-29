param([string]$Exe = (Join-Path $PSScriptRoot '../build/little-e2e.exe'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$executable = (Resolve-Path $Exe).Path
$crlf = [string][char]13 + [string][char]10

$textBytes = [Text.Encoding]::UTF8.GetBytes('hello compressed')
$memory = [IO.MemoryStream]::new()
$gzip = [IO.Compression.GZipStream]::new($memory, [IO.Compression.CompressionMode]::Compress)
$gzip.Write($textBytes, 0, $textBytes.Length)
$gzip.Dispose()
$compressed = $memory.ToArray()

function Run-Fixture([string]$Name, [string]$Source, [string]$Expected) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $process = $null
    $client = $null
    try {
        $listener.Start()
        $port = $listener.LocalEndpoint.Port
        $fixture = Join-Path $repo "build/web-decompress-$Name.little"
        [IO.Directory]::CreateDirectory((Split-Path -Parent $fixture)) | Out-Null
        [IO.File]::WriteAllText($fixture, $Source.Replace('PORT', $port.ToString()), [Text.UTF8Encoding]::new($false))
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
        $buffer = [byte[]]::new(4096)
        $request = [Text.StringBuilder]::new()
        $terminator = $crlf + $crlf
        while (-not $request.ToString().Contains($terminator)) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if ($count -eq 0) { throw 'Client closed before request headers' }
            [void]$request.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $count))
        }
        $header = "HTTP/1.1 200 OK" + $crlf + "Content-Encoding: gzip" + $crlf + "Content-Length: $($compressed.Length)" + $crlf + "Connection: close" + $crlf + $crlf
        $headerBytes = [Text.Encoding]::ASCII.GetBytes($header)
        $stream.Write($headerBytes, 0, $headerBytes.Length)
        $stream.Write($compressed, 0, $compressed.Length)
        $client.Close()
        $client = $null

        if (-not $process.WaitForExit(10000)) { throw 'Timed out waiting for Little to exit' }
        $output = $stdout.Result.Replace($crlf, [string][char]10).TrimEnd()
        $errors = $stderr.Result.Trim()
        if ($process.ExitCode -ne 0 -or $errors -or $output -ne $Expected) {
            $message = "Output mismatch (exit $($process.ExitCode)):" + [char]10 + $output + [char]10 + "Errors:" + [char]10 + $errors
            throw $message
        }
        Write-Host "PASS decompress $Name"
    }
    finally {
        if ($client) { $client.Close() }
        $listener.Stop()
        if ($process) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
}

$enabled = @'
web.fetch("http://127.0.0.1:PORT/data", { decompress: true }).next(fn(response) {
    io.print(response.body is "hello compressed")
}).catch(fn(reason) {
    io.print(reason)
})
'@
$disabled = @'
web.fetch("http://127.0.0.1:PORT/data", { decompress: false }).next(fn(response) {
    io.print("unexpected body")
}).catch(fn(reason) {
    io.print(reason)
})
'@

Run-Fixture 'enabled' $enabled 'true'
Run-Fixture 'disabled' $disabled 'Binary HTTP response is not supported'
