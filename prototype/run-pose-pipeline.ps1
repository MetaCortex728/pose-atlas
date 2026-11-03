param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Blender = 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe',
    [string]$Aseprite = 'C:\Program Files\Aseprite\Aseprite.exe',
    [string]$Python = 'python',
    [string]$SourceBlend = '',
    [ValidateSet(1, 8)][int]$Views = 1,
    [ValidateSet(64, 96)][int]$CellSize = 64,
    [switch]$Walk,
    [switch]$NormalMaps
)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
$gameRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) + '\'
if (-not $output.StartsWith($gameRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Generated art must stay inside this game workspace.'
}
foreach ($file in @($Blender, $Aseprite, $Python)) {
    if ([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($file)) -ne 'C:\' -or -not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing tool on authorized C: volume: $file"
    }
}
if (Test-Path -LiteralPath $output) { throw 'Choose a fresh output directory; existing artwork is preserved.' }
$renderArgs = @('--background', '--factory-startup', '--python-exit-code', '1', '--python', (Join-Path $PSScriptRoot 'render-pose-proxy.py'), '--', '--out', $output, '--views', $Views, '--cell-size', $CellSize)
if ($Walk) { $renderArgs += '--walk' }
if ($SourceBlend) {
    if ([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($SourceBlend)) -ne 'C:\') { throw 'Only C: inputs are authorized.' }
    $renderArgs += @('--source', (Resolve-Path -LiteralPath $SourceBlend).Path)
}
if ($NormalMaps) {
    if (-not $SourceBlend -or $Views -ne 8 -or -not $Walk) { throw 'NormalMaps requires SourceBlend, Views 8 and Walk.' }
    $contract = Join-Path (Split-Path (Resolve-Path -LiteralPath $SourceBlend).Path -Parent) 'pose-contract.json'
    $renderArgs = @('--background', '--factory-startup', '--python-exit-code', '1', '--python', (Join-Path $PSScriptRoot 'render-normal-probe.py'), '--',
        '--source', (Resolve-Path -LiteralPath $SourceBlend).Path, '--out', $output, '--cell-size', $CellSize, '--atlas-contract', $contract)
}
& $Blender @renderArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $output 'pose-contract.json'))) { throw 'Blender did not complete the render contract.' }
& $Aseprite --batch --script-param "root=$($output.Replace('\','/'))" --script (Join-Path $PSScriptRoot 'import-pose-proxy.lua') 2>&1 | Out-String | Write-Output
if ($LASTEXITCODE -ne 0) { throw "Aseprite import failed: $LASTEXITCODE" }
$renderContract = Get-Content -LiteralPath (Join-Path $output 'pose-contract.json') -Raw | ConvertFrom-Json
$columns = $renderContract.pose_names.Count
if ($columns -notin @(5, 9, 13, 21, 25, 57)) { throw 'Unsupported pose layout.' }
& $Aseprite --batch (Join-Path $output 'pose-proxy.aseprite') --sheet-type rows --sheet-columns $columns --format json-array --list-tags --list-slices --sheet (Join-Path $output 'pose-proxy.png') --data (Join-Path $output 'pose-proxy.json') 2>&1 | Out-String | Write-Output
if ($LASTEXITCODE -ne 0) { throw "Aseprite export failed: $LASTEXITCODE" }
foreach ($name in @('pose-proxy.aseprite', 'pose-proxy.png', 'pose-proxy.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $output $name))) { throw "Missing output: $name" }
}
& $Python (Join-Path $PSScriptRoot 'check-pose-pipeline.py') $output --report (Join-Path $output 'validation.json')
if ($LASTEXITCODE -ne 0) { throw 'Pose metadata/pixel validation failed; output is not ready for import.' }
if ($NormalMaps) {
    & $Aseprite --batch --script-param "root=$($output.Replace('\','/'))" --script-param 'normals=true' --script (Join-Path $PSScriptRoot 'import-pose-proxy.lua') 2>&1 | Out-String | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Normal source import failed.' }
    & $Aseprite --batch (Join-Path $output 'normal-proxy.aseprite') --sheet-type rows --sheet-columns $columns --format json-array --list-tags --list-slices --sheet (Join-Path $output 'normal-proxy.png') --data (Join-Path $output 'normal-proxy.json') 2>&1 | Out-String | Write-Output
    if ($LASTEXITCODE -ne 0) { throw 'Normal atlas export failed.' }
    & $Python (Join-Path $PSScriptRoot 'check-normal-atlas.py') $output --output (Join-Path $output 'normal-validation.json')
    if ($LASTEXITCODE -ne 0) { throw 'Paired normal atlas validation failed.' }
}
Write-Output "Produced editable proxy and fixed-cell atlas, not final character art: $output"
