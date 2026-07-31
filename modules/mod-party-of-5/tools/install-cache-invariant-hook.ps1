# Installs server-ac's pre-commit guard for the party-member loadout-cache
# invariant (check-cache-invariant.py). Re-run after a fresh clone -- git hooks
# are not version-controlled.
#
# server-ac is a SEPARATE git repo from D:\WowPs, so D:\WowPs's addon pre-commit
# hook never sees a commit made here. This is the server-side equivalent.
#
# Two pieces, mirroring D:\WowPs\install-addon-test-hook.ps1:
#   1. .git/hooks/pre-commit -- this repo's own hook.
#   2. a global dispatcher at ~/.claude/git-hooks/pre-commit, because this
#      machine sets core.hooksPath globally, which makes git ignore every
#      repo's .git/hooks entirely. Without the dispatcher the hook below is
#      dead code that silently never fires.

$ErrorActionPreference = 'Stop'

# tools/ -> mod-party-of-5/ -> modules/ -> server-ac/
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo     = Resolve-Path (Join-Path $toolsDir '..\..\..')
$hookDir  = Join-Path $repo '.git\hooks'
$hookPath = Join-Path $hookDir 'pre-commit'

if (-not (Test-Path (Join-Path $repo '.git'))) {
    throw "no .git at $repo -- run this from inside the server-ac checkout"
}
New-Item -ItemType Directory -Force $hookDir | Out-Null

$hook = @'
#!/bin/sh
# Refuse a commit that breaks the party-member loadout-cache invariant: a member
# spawned without its party_loadout row loaded into the runtime caches fights
# with NO rules, silently (2026-07-31, the roster-invite bug). Costs nothing on
# commits that touch no mod-party-of-5 C++.
staged=$(git diff --cached --name-only --diff-filter=ACM |
         grep -E '^modules/mod-party-of-5/src/.*\.(cpp|h)$')
[ -z "$staged" ] && exit 0

repo_root=$(git rev-parse --show-toplevel)
python "$repo_root/modules/mod-party-of-5/tools/check-cache-invariant.py"
status=$?
if [ $status -ne 0 ]; then
    echo ""
    echo "pre-commit: fix the invariant, do NOT reach for --no-verify."
    echo "            re-run: python modules/mod-party-of-5/tools/check-cache-invariant.py --list"
fi
exit $status
'@

# LF endings and no BOM: sh chokes on both.
[System.IO.File]::WriteAllText($hookPath, ($hook -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "installed $hookPath"

$globalHooks = git config --get core.hooksPath
if ($globalHooks) {
    $expanded = $globalHooks -replace '^~', $HOME
    $dispatch = Join-Path $expanded 'pre-commit'
    if (Test-Path $dispatch) {
        Write-Host "global dispatcher already present at $dispatch"
    } else {
        $body = @'
#!/bin/sh
# Global dispatcher: core.hooksPath shadows every repo's .git/hooks, so
# per-repo pre-commit hooks silently never ran. Delegate to the repo's own
# hook when it exists; no-op otherwise.
repo_hook="$(git rev-parse --git-dir)/hooks/pre-commit"
[ -x "$repo_hook" ] && exec "$repo_hook" "$@"
exit 0
'@
        [System.IO.File]::WriteAllText($dispatch, ($body -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "installed global dispatcher $dispatch"
    }
}

Write-Host ""
Write-Host "verifying the guard runs..."
python (Join-Path $toolsDir 'check-cache-invariant.py')
