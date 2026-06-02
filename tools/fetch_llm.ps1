# Downloads a local llama.cpp server (CPU build) and a small instruct model into
# the game's llm/ folder so the bundled LLM works with no manual install.
#
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File tools\fetch_llm.ps1
#   powershell -ExecutionPolicy Bypass -File tools\fetch_llm.ps1 -Dest build\bin\llm
param(
    [string]$Dest = "build\bin\llm",
    [string]$ModelUrl = "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$tmp = Join-Path $env:TEMP "llmbot_fetch"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

Write-Host "Resolving latest llama.cpp CPU (win-x64) release..."
$rel = Invoke-RestMethod -Uri "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest" -Headers @{ "User-Agent" = "fetch-llm" }
$asset = $rel.assets | Where-Object { $_.name -match "win-cpu-x64\.zip$" } | Select-Object -First 1
if (-not $asset) { throw "Could not find win-cpu-x64 asset in release $($rel.tag_name)" }
Write-Host "  $($rel.tag_name): $($asset.name) ($([math]::Round($asset.size/1MB,1)) MB)"

$zip = Join-Path $tmp "llama-cpu.zip"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
$extract = Join-Path $tmp "llama"
if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $extract -Force

# The zip may put binaries at the root or in a subfolder; find llama-server.exe.
$server = Get-ChildItem -Path $extract -Recurse -Filter "llama-server.exe" | Select-Object -First 1
if (-not $server) { throw "llama-server.exe not found in the downloaded archive" }
$binDir = $server.Directory.FullName
Write-Host "Copying server + runtime DLLs from $binDir ..."
Copy-Item -Path (Join-Path $binDir "*") -Destination $Dest -Recurse -Force

Write-Host "Downloading model:"
Write-Host "  $ModelUrl"
$model = Join-Path $Dest "model.gguf"
Invoke-WebRequest -Uri $ModelUrl -OutFile $model -UseBasicParsing
$mb = [math]::Round((Get-Item $model).Length / 1MB, 1)
Write-Host "Model saved: $model ($mb MB)"

Write-Host ""
Write-Host "Done. Files in ${Dest}:"
Get-ChildItem $Dest | ForEach-Object { Write-Host ("  " + $_.Name) }
Write-Host ""
Write-Host "Now set in config.json:  llm_enabled=true, llm_autostart_server=true"
