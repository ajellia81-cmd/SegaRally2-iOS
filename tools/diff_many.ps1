# Runs --diff for several functions and compares each one with Unicorn.
# Usage: powershell -File tools/diff_many.ps1 -Frame 3000 -Functions 10D564,33148
param([int]$Frame = 3000, [string]$Functions)
Set-Location (Split-Path $PSScriptRoot -Parent)
foreach ($fn in $Functions.Split(",")) {
    $p = Start-Process -FilePath .\SegaRally2.exe -ArgumentList "--headless", "--frames", "6000", "--auto",
        "coin@1500,start@1700", "--diff", "$fn@$Frame" -NoNewWindow -PassThru -RedirectStandardOutput out\diff_run.txt -RedirectStandardError out\diff_err.txt
    $null = $p.WaitForExit(300000)
    $summary = Get-Content out\diff_run.txt | Select-String "diff:"
    $job = Start-Job -ScriptBlock { param($d) Set-Location $d; python -I tools/diff_unicorn.py out data 2>&1 } -ArgumentList (Get-Location).Path
    if (-not (Wait-Job $job -Timeout 120)) { Stop-Job $job; "fn_$fn : Unicorn did not finish in 120 s" ; continue }
    $out = Receive-Job $job
    "==== fn_$fn  $summary"
    $out | Select-String "Ended at|Differences|  r[0-9]|  f[0-9]|  CR|  XER|  CTR|RAM 0" | Select-Object -First 14
}
