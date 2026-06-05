# upload.ps1 — upload a file to TestHttpServer
# usage: .\upload.ps1 <file> [host:port] [remote_name]
param(
    [Parameter(Mandatory)][string]$File,
    [string]$Server = "127.0.0.1:9000",
    [string]$Remote = ""
)

if (-not (Test-Path $File -PathType Leaf)) {
    Write-Error "file not found: $File"
    exit 1
}

if ($Remote -eq "") {
    $Remote = Split-Path $File -Leaf
}

curl.exe -fS -T $File "http://$Server/upload?path=$Remote"
