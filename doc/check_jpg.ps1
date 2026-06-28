param([string]$path)
$b = [System.IO.File]::ReadAllBytes($path)
$markers = @()
$limit = [Math]::Min($b.Length-1, 600000)
for ($i=0; $i -lt $limit; $i++) {
    if ($b[$i] -eq 0xFF) {
        $m = $b[$i+1]
        if ($m -ne 0x00 -and $m -ne 0xFF -and ($m -lt 0xD0 -or $m -gt 0xD7)) {
            $markers += ('FF{0:X2}@{1}' -f $m, $i)
        }
    }
}
Write-Output ("File=" + $path)
Write-Output ("Size=" + $b.Length)
Write-Output ("First20=" + (($b[0..19] | ForEach-Object { $_.ToString('X2') }) -join ' '))
Write-Output ("Last4=" + (($b[($b.Length-4)..($b.Length-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '))
Write-Output ("Markers=" + ($markers -join ' '))
$hasDHT = $markers | Where-Object { $_ -like 'FFC4*' }
$hasDQT = $markers | Where-Object { $_ -like 'FFDB*' }
$hasSOF = $markers | Where-Object { $_ -like 'FFC0*' -or $_ -like 'FFC2*' }
Write-Output ("HasDQT=" + [bool]$hasDQT + " HasDHT=" + [bool]$hasDHT + " HasSOF=" + [bool]$hasSOF)
