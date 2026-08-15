param(
    [string]$Exe = "./build/little.exe"
)

$ErrorActionPreference = "Stop"

function Invoke-Little([string[]]$Arguments) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Exe @Arguments 2>&1 | Out-String
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return [PSCustomObject]@{
        Output = $output.TrimEnd()
        ExitCode = $LASTEXITCODE
    }
}

function Assert-Result($Result, [int]$ExitCode, [string]$Text) {
    if ($Result.ExitCode -ne $ExitCode -or !$Result.Output.Contains($Text)) {
        throw "Expected exit $ExitCode and '$Text', got exit $($Result.ExitCode): $($Result.Output)"
    }
}

Assert-Result (Invoke-Little -Arguments @("--help")) 0 "Usage: little"
Assert-Result (Invoke-Little -Arguments @("--version")) 0 "little API"
Assert-Result (Invoke-Little -Arguments @("-e", "io.print(7)")) 0 "7.000000"
Assert-Result (Invoke-Little -Arguments @("-I", "tests/fixtures", "tests/fixtures/cli-import.little")) 0 "hello CLI"
Assert-Result (Invoke-Little -Arguments @("-e", "var value = 1`nvalue()")) 1 "Value is not callable!"
Assert-Result (Invoke-Little -Arguments @("does-not-exist.little")) 2 "Failed to open"
Assert-Result (Invoke-Little -Arguments @("-I")) 2 "-I requires a directory"
Assert-Result (Invoke-Little -Arguments @("--unknown")) 2 "Unknown option"

Write-Host "CLI tests passed."
