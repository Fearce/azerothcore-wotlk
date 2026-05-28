# test-protocol.ps1
#
# End-to-end test harness for the mod-party-of-5 server-side protocol layer.
# Drives the worldserver via SOAP (admin commands), drives the DB via direct
# MySQL, and asserts state changes after each call.
#
# Coverage:
#   * BOOTSTRAP_PARTY (WPSP handler logic) - via .wowps_admin bootstrap
#   * Party SQL surface - direct DB queries
#
# NOT covered (would require a real WoW client session):
#   * PET_BAR_SET / GOTO_DELTA - need an active possess
#   * Addon UI (Lua/XML behavior, drag capture, key bindings)
#
# Usage: pwsh -File .\scripts\test-protocol.ps1
# Exits 0 on all PASS, 1 on any FAIL.

$ErrorActionPreference = 'Stop'

$SOAP_URL  = 'http://127.0.0.1:7878/'
$SOAP_USER = 'KEV'
$SOAP_PASS = 'password'

$TEST_ACCOUNT = 9001

$Total = 0
$Failed = 0

function Norm {
    param($s)
    if ($null -eq $s) { return '' }
    return ($s.ToString() -replace "`r`n", "`n" -replace "`r", "`n").Trim()
}

function Assert-Equal {
    param([string]$Label, $Expected, $Actual)
    $script:Total++
    $e = Norm $Expected
    $a = Norm $Actual
    if ($e -ceq $a) {
        Write-Host "PASS  $Label" -ForegroundColor Green
    } else {
        $script:Failed++
        Write-Host "FAIL  $Label" -ForegroundColor Red
        Write-Host "      expected: $(($e -replace "`n", '\n'))" -ForegroundColor DarkGray
        Write-Host "      actual:   $(($a -replace "`n", '\n'))" -ForegroundColor DarkGray
    }
}

function Invoke-Soap {
    param([string]$Command)
    $envelope = "<?xml version=`"1.0`" encoding=`"UTF-8`"?><SOAP-ENV:Envelope xmlns:SOAP-ENV=`"http://schemas.xmlsoap.org/soap/envelope/`" xmlns:ns1=`"urn:AC`"><SOAP-ENV:Body><ns1:executeCommand><command>$Command</command></ns1:executeCommand></SOAP-ENV:Body></SOAP-ENV:Envelope>"
    $pair  = "${SOAP_USER}:${SOAP_PASS}"
    $b64   = [Convert]::ToBase64String([System.Text.Encoding]::ASCII.GetBytes($pair))
    $hdrs  = @{ Authorization = "Basic $b64"; 'Content-Type' = 'text/xml; charset=utf-8' }
    try {
        $resp = Invoke-WebRequest -Uri $SOAP_URL -Method POST -Headers $hdrs -Body $envelope -UseBasicParsing -TimeoutSec 10
        return [string]$resp.Content
    } catch {
        if ($_.Exception.Response) {
            $stream = $_.Exception.Response.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            return $reader.ReadToEnd()
        }
        throw
    }
}

function Invoke-MySQL {
    param([string]$Sql)
    # MYSQL_PWD env var avoids the "password on command line is insecure"
    # warning that gets piped to stderr and causes PS 5.1's ErrorAction=Stop
    # to abort on the next call.
    $out = & docker exec -e MYSQL_PWD=password ac-database mysql -uroot -N -B -e $Sql 2>$null
    return (($out | Out-String).Trim())
}

# Test 1: SOAP connectivity
Write-Host ""
Write-Host "=== Test 1: SOAP connectivity ===" -ForegroundColor Cyan
$serverInfo = Invoke-Soap '.server info'
$hasReady = $serverInfo -match 'AzerothCore'
Assert-Equal 'soap returns server info' $true $hasReady

# Test 2: bootstrap on empty account = 0 enrolled
Write-Host ""
Write-Host "=== Test 2: bootstrap on empty account ===" -ForegroundColor Cyan
Invoke-MySQL "DELETE FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT; DELETE FROM acore_characters.characters WHERE guid IN (9001,9002,9003);" | Out-Null

$resp = Invoke-Soap ".wowps_admin bootstrap $TEST_ACCOUNT"
$enrolled = if ($resp -match 'enrolled=(\d+)') { [int]$Matches[1] } else { -1 }
Assert-Equal 'bootstrap on empty account: enrolled count' 0 $enrolled

$dbCount = [int](Invoke-MySQL "SELECT COUNT(*) FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT;")
Assert-Equal 'bootstrap on empty account: DB row count' 0 $dbCount

# Test 3: bootstrap with 3 unenrolled chars enrolls all 3 at slots 0,1,2
Write-Host ""
Write-Host "=== Test 3: bootstrap enrolls 3 unenrolled chars ===" -ForegroundColor Cyan

$insertChars = "DELETE FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT; DELETE FROM acore_characters.characters WHERE guid IN (9001,9002,9003); INSERT INTO acore_characters.characters (guid, account, name, race, class, gender, level, taximask, innTriggerId, exploredZones, equipmentCache, knownTitles) VALUES (9001, $TEST_ACCOUNT, 'TestAlpha', 1, 1, 0, 1, '0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0', 0, '', '', ''), (9002, $TEST_ACCOUNT, 'TestBeta', 1, 2, 0, 1, '0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0', 0, '', '', ''), (9003, $TEST_ACCOUNT, 'TestGamma', 1, 5, 1, 1, '0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0', 0, '', '', '');"
Invoke-MySQL $insertChars | Out-Null

$resp = Invoke-Soap ".wowps_admin bootstrap $TEST_ACCOUNT"
$enrolled = if ($resp -match 'enrolled=(\d+)') { [int]$Matches[1] } else { -1 }
Assert-Equal 'bootstrap enrolls 3 chars: enrolled count' 3 $enrolled

$dbRows = Invoke-MySQL "SELECT slot, guid FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT ORDER BY slot;"
Assert-Equal 'bootstrap: DB row layout' "0`t9001`n1`t9002`n2`t9003" $dbRows

$activeCount = [int](Invoke-MySQL "SELECT COUNT(*) FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT AND is_active_on_login = 1;")
Assert-Equal 'bootstrap: exactly one active-on-login' 1 $activeCount

$activeSlot = [int](Invoke-MySQL "SELECT slot FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT AND is_active_on_login = 1;")
Assert-Equal 'bootstrap: active-on-login is slot 0' 0 $activeSlot

$denorm = Invoke-MySQL "SELECT guid, party_slot FROM acore_characters.characters WHERE guid IN (9001,9002,9003) ORDER BY guid;"
Assert-Equal 'bootstrap: characters.party_slot denormalised' "9001`t0`n9002`t1`n9003`t2" $denorm

# Test 4: bootstrap idempotent
Write-Host ""
Write-Host "=== Test 4: bootstrap is idempotent ===" -ForegroundColor Cyan
$resp = Invoke-Soap ".wowps_admin bootstrap $TEST_ACCOUNT"
$enrolled2 = if ($resp -match 'enrolled=(\d+)') { [int]$Matches[1] } else { -1 }
Assert-Equal 'bootstrap second run: no new enrollments' 0 $enrolled2

$rowsAfter = [int](Invoke-MySQL "SELECT COUNT(*) FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT;")
Assert-Equal 'bootstrap second run: DB unchanged' 3 $rowsAfter

# Test 5: verify_party admin command reflects state
Write-Host ""
Write-Host "=== Test 5: verify_party reflects DB state ===" -ForegroundColor Cyan
$resp = Invoke-Soap ".wowps_admin verify_party $TEST_ACCOUNT"
$reportedRows = if ($resp -match 'rows=(\d+)') { [int]$Matches[1] } else { -1 }
Assert-Equal 'verify_party reports 3 rows' 3 $reportedRows

# Test 6: in-process handler test runner against real bot Players.
# Spawns 3 mod-playerbots autonomous bots and drives every handler
# (spellbook dedupe, GotoDelta, PetBarSet) against them. Results land
# in handler-test-results.log next to worldserver.exe.
Write-Host ""
Write-Host "=== Test 6: in-process handler tests ===" -ForegroundColor Cyan
$resultsPath = 'D:\WowPs\server-ac-build\bin\RelWithDebInfo\handler-test-results.log'
Remove-Item $resultsPath -ErrorAction SilentlyContinue

# First fire: may report "bots not yet loaded" — that's the AddPlayerBot kick.
Invoke-Soap '.wowps_admin run_handler_tests' | Out-Null
# Wait up to 30s for the bots to come online + tests to run
$deadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Seconds 2
    if (Test-Path $resultsPath) {
        $content = Get-Content $resultsPath -Raw
        if ($content -match 'TOTAL=' -and $content -notmatch 'FAIL.*not yet loaded') { break }
    }
    # Re-fire to actually run tests now that bots are loading/loaded
    Invoke-Soap '.wowps_admin run_handler_tests' | Out-Null
} while ((Get-Date) -lt $deadline)
# Give the deferred PetBarSet 3s to append
Start-Sleep -Seconds 3

if (Test-Path $resultsPath) {
    $lines = Get-Content $resultsPath
    foreach ($l in $lines) { Write-Host "  $l" -ForegroundColor DarkGray }
    $summary = $lines | Where-Object { $_ -match '^TOTAL=' } | Select-Object -Last 1
    if ($summary -match 'TOTAL=(\d+) PASSED=(\d+) FAILED=(\d+)') {
        $hTot = [int]$Matches[1]; $hPass = [int]$Matches[2]; $hFail = [int]$Matches[3]
        # Include the deferred PetBarSet (appended after summary) in totals
        $extraPass = ($lines | Where-Object { $_ -match '^PASS.*PetBarSet' }).Count
        $extraFail = ($lines | Where-Object { $_ -match '^FAIL.*PetBarSet' }).Count
        $hTot += ($extraPass + $extraFail)
        $hPass += $extraPass
        $hFail += $extraFail
        $script:Total += $hTot
        $script:Failed += $hFail
        $clr = if ($hFail -eq 0) { 'Green' } else { 'Red' }
        Write-Host "  handler tests: $hPass/$hTot passed" -ForegroundColor $clr
    } else {
        $script:Total++; $script:Failed++
        Write-Host "  FAIL: results file present but no TOTAL summary" -ForegroundColor Red
    }
} else {
    $script:Total++; $script:Failed++
    Write-Host "  FAIL: handler-test-results.log never appeared (bots didn't spawn?)" -ForegroundColor Red
}

# Cleanup
Write-Host ""
Write-Host "=== Cleanup ===" -ForegroundColor Cyan
Invoke-MySQL "DELETE FROM acore_characters.account_party WHERE account = $TEST_ACCOUNT; DELETE FROM acore_characters.characters WHERE guid IN (9001,9002,9003);" | Out-Null
Write-Host "Cleaned test account $TEST_ACCOUNT"

# Summary
Write-Host ""
Write-Host "=== Summary ===" -ForegroundColor Cyan
$passed = $Total - $Failed
if ($Failed -gt 0) {
    Write-Host "Total: $Total  Passed: $passed  Failed: $Failed" -ForegroundColor Red
    exit 1
} else {
    Write-Host "Total: $Total  Passed: $passed  Failed: $Failed" -ForegroundColor Green
    exit 0
}
