$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$destination = Join-Path $repo 'Automation\output\renderdoc-src'
if (-not (Test-Path -LiteralPath (Join-Path $destination '.git'))) {
    git clone --depth 1 --filter=blob:none --sparse --branch v1.x https://github.com/baldurk/renderdoc.git $destination
    if ($LASTEXITCODE -ne 0) { throw 'RenderDoc clone failed.' }
}
git -C $destination sparse-checkout set renderdoc/api/app docs
if ($LASTEXITCODE -ne 0) { throw 'RenderDoc sparse checkout failed.' }
git -C $destination rev-parse HEAD

