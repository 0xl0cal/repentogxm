<#
Fixture plus sealed-evidence gate for the opt-in Isaac runtime-health contract.
The runner is parsed and only its classifier function is evaluated; Vita3K is
never started and every consumed real log is rehashed before classification.
#>
[CmdletBinding()]
param(
    [string]$Runner = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Runner) {
    $Runner = Join-Path $PSScriptRoot 'run_vita3k_diagnostic.ps1'
}
$runnerPath = (Resolve-Path -LiteralPath $Runner).Path
$tokens = $null
$parseErrors = $null
$runnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Runner parse failed: $($parseErrors[0].Message)"
}

$healthSwitch = @($runnerAst.ParamBlock.Parameters | Where-Object {
    $_.Name.VariablePath.UserPath -eq 'RequireIsaacRuntimeHealth'
})
if ($healthSwitch.Count -ne 1 -or
        $healthSwitch[0].StaticType -ne [System.Management.Automation.SwitchParameter]) {
    throw 'Runner does not expose exactly one opt-in Isaac health switch.'
}
$floorLifeSwitch = @($runnerAst.ParamBlock.Parameters | Where-Object {
    $_.Name.VariablePath.UserPath -eq 'RequireIsaacFloorLifetime'
})
if ($floorLifeSwitch.Count -ne 1 -or
        $floorLifeSwitch[0].StaticType -ne
            [System.Management.Automation.SwitchParameter]) {
    throw 'Runner does not expose exactly one opt-in floor-lifetime switch.'
}
if ($runnerAst.ParamBlock.Parameters[-1].Name.VariablePath.UserPath -ne
        'RequireIsaacFloorLifetime' -or
        $runnerAst.ParamBlock.Parameters[-2].Name.VariablePath.UserPath -ne
        'RequireIsaacRuntimeHealth') {
    throw 'Opt-in health switches must remain trailing positional parameters.'
}

$healthFunctions = @($runnerAst.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-IsaacRuntimeHealthContract'
}, $true))
if ($healthFunctions.Count -ne 1) {
    throw "Expected one runtime-health helper, found $($healthFunctions.Count)."
}
$healthAst = $healthFunctions[0]
$forbiddenCommands = @($healthAst.FindAll({
    param($node)
    if ($node -isnot [System.Management.Automation.Language.CommandAst]) {
        return $false
    }
    return $node.GetCommandName() -in @(
        'Get-Content', 'Set-Content', 'Test-Path', 'Start-Process',
        'Get-Process', 'Stop-Process')
}, $true))
if ($forbiddenCommands.Count -ne 0) {
    throw 'Runtime-health helper is not pure evidence-in/evidence-out code.'
}
. ([scriptblock]::Create($healthAst.Extent.Text))

function Invoke-HealthFixture {
    param(
        [string]$Stdout = '',
        [string]$Native = '',
        [string]$Game = '',
        [bool]$Required = $true,
        [bool]$FloorLifeRequired = $false
    )
    return Get-IsaacRuntimeHealthContract `
        -CurrentStdout $Stdout `
        -CurrentNativeLog $Native `
        -CurrentGameLog $Game `
        -Required $Required `
        -FloorLifeRequired $FloorLifeRequired
}

function Assert-Healthy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [string]$Stdout = '',
        [string]$Native = '',
        [string]$Game = '',
        [bool]$FloorLifeRequired = $false
    )
    $result = Invoke-HealthFixture -Stdout $Stdout -Native $Native `
        -Game $Game -FloorLifeRequired $FloorLifeRequired
    if (-not $result.Passed -or $result.FailureCount -ne 0) {
        throw "$Name was rejected: $($result.Failure)"
    }
    return $result
}

function Assert-FailureCode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Code,
        [string]$Stdout = '',
        [string]$Native = '',
        [string]$Game = '',
        [bool]$FloorLifeRequired = $false,
        [switch]$PassThru
    )
    $result = Invoke-HealthFixture -Stdout $Stdout -Native $Native `
        -Game $Game -FloorLifeRequired $FloorLifeRequired
    if ($result.Passed -or $result.FailureCodes -notcontains $Code) {
        throw "$Name did not produce $Code`: $($result.Failure)"
    }
    if ($PassThru) { return $result }
}

$stageMemoryHealthyTail =
    'mi=1000/900/700/5 ledger=9 ov=3/1234/2/20000 ' +
    'slab=2/2/0/11/14/3 valid/term/sat=1/0/0'
function New-StageMemoryRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Q,
        [Parameter(Mandatory = $true)][string]$LevelQ,
        [Parameter(Mandatory = $true)][string]$RoomQ,
        [Parameter(Mandatory = $true)][string]$Event,
        [Parameter(Mandatory = $true)][string]$Active,
        [Parameter(Mandatory = $true)][string]$LevelStage,
        [Parameter(Mandatory = $true)][string]$LevelType,
        [Parameter(Mandatory = $true)][string]$RoomStage,
        [Parameter(Mandatory = $true)][string]$RoomMode,
        [Parameter(Mandatory = $true)][string]$Result
    )
    return 'stagemem: q=' + $Q + ' lq/rq=' + $LevelQ + '/' + $RoomQ +
        ' e=' + $Event + ' ctx_valid=1 active=' + $Active +
        ' ls=' + $LevelStage + ' lt=' + $LevelType +
        ' rs=' + $RoomStage + ' mode=' + $RoomMode +
        ' ok=' + $Result + ' ' + $stageMemoryHealthyTail
}

$healthyStageNominal = @(
    (New-StageMemoryRecord '1' '1' '0' 'level-begin' '1' `
        '3' '2' 'ffffffff' 'ffffffff' 'ffffffff'),
    (New-StageMemoryRecord '2' '1' '2' 'room-reuse' '1' `
        '3' '2' '0' '1' 'ffffffff'),
    (New-StageMemoryRecord '3' '1' '3' 'room-load-begin' '1' `
        '3' '2' '4' '1' 'ffffffff'),
    (New-StageMemoryRecord '4' '1' '3' 'room-after-unload' '1' `
        '3' '2' '4' '1' 'ffffffff'),
    (New-StageMemoryRecord '5' '1' '3' 'room-load-end' '1' `
        '3' '2' '4' '1' '1'),
    (New-StageMemoryRecord '6' '1' '3' 'level-end' '1' `
        '3' '2' '4' '1' 'ffffffff')
)
$healthyStageNominalText = $healthyStageNominal -join "`n"
$healthyStageEarlyFailure = @(
    (New-StageMemoryRecord '1' '0' '1' 'room-load-begin' '0' `
        'ffffffff' 'ffffffff' '7' '2' 'ffffffff'),
    (New-StageMemoryRecord '2' '0' '1' 'room-load-end' '0' `
        'ffffffff' 'ffffffff' '7' '2' '0')
)
$healthyStageEarlyFailureText = $healthyStageEarlyFailure -join "`n"

$stageNominalResult = Assert-Healthy -Name 'stage-memory nominal lifecycle' `
    -Stdout $healthyStageNominalText
if ($stageNominalResult.StageMemoryAllSourceRecords -ne 6 -or
        $stageNominalResult.StageMemoryAuthoritativeSource -ne 'stdout' -or
        $stageNominalResult.StageMemoryAuthoritativeRecords -ne 6 -or
        $stageNominalResult.StageMemoryLevelPairs -ne 1 -or
        $stageNominalResult.StageMemoryLoadSuccessPairs -ne 1 -or
        $stageNominalResult.StageMemoryLoadFailurePairs -ne 0 -or
        $stageNominalResult.StageMemoryReuseRecords -ne 1) {
    throw 'Healthy nominal stage-memory counters were not preserved.'
}
$stageReorderedResult = Assert-Healthy `
    -Name 'stage-memory q order overrides physical order' `
    -Native (@($healthyStageEarlyFailure[1],
        $healthyStageEarlyFailure[0]) -join "`n")
if ($stageReorderedResult.StageMemoryAuthoritativeSource -ne 'native' -or
        $stageReorderedResult.StageMemoryAuthoritativeRecords -ne 2 -or
        $stageReorderedResult.StageMemoryLoadFailurePairs -ne 1) {
    throw 'Reordered stage-memory failure pair was not reconstructed by q.'
}
$stageRaceResult = Assert-Healthy `
    -Name 'stage-memory pre-stop stdout ends before native' `
    -Stdout ($healthyStageNominal[0..4] -join "`n") `
    -Native $healthyStageNominalText
if ($stageRaceResult.StageMemoryAllSourceRecords -ne 11 -or
        $stageRaceResult.StageMemoryAuthoritativeSource -ne 'native' -or
        $stageRaceResult.StageMemoryAuthoritativeRecords -ne 6 -or
        $stageRaceResult.StageMemoryLevelPairs -ne 1) {
    throw 'Native stage-memory authority did not tolerate a lower prefix.'
}
$healthyStageFailureAfterUnload = @(
    (New-StageMemoryRecord '1' '0' '1' 'room-load-begin' '0' `
        'ffffffff' 'ffffffff' '7' '2' 'ffffffff'),
    (New-StageMemoryRecord '2' '0' '1' 'room-after-unload' '0' `
        'ffffffff' 'ffffffff' '7' '2' 'ffffffff'),
    (New-StageMemoryRecord '3' '0' '1' 'room-load-end' '0' `
        'ffffffff' 'ffffffff' '7' '2' '0')
) -join "`n"
$stageFailureAfterUnloadResult = Assert-Healthy `
    -Name 'stage-memory failure after unload remains valid' `
    -Stdout $healthyStageFailureAfterUnload
if ($stageFailureAfterUnloadResult.StageMemoryLoadFailurePairs -ne 1) {
    throw 'Failure result after unload was not counted as a valid pair.'
}

$stageTruncatedNative = Assert-FailureCode `
    -Name 'truncated native stage marker does not fall back' `
    -Code 'stagemem-format' -Stdout $healthyStageNominalText `
    -Native 'stagemem: q=1' -PassThru
if ($stageTruncatedNative.StageMemoryAuthoritativeSource -ne 'native' -or
        $stageTruncatedNative.StageMemoryAuthoritativeRecords -ne 1) {
    throw 'Truncated native stage marker incorrectly fell back to stdout.'
}
$stageUppercaseNative = Assert-FailureCode `
    -Name 'uppercase native stage marker does not fall back' `
    -Code 'stagemem-format' -Stdout $healthyStageNominalText `
    -Native $healthyStageNominal[0].Replace('stagemem:', 'STAGEMEM:') `
    -PassThru
if ($stageUppercaseNative.StageMemoryAuthoritativeSource -ne 'native' -or
        $stageUppercaseNative.StageMemoryAuthoritativeRecords -ne 1) {
    throw 'Uppercase native stage marker incorrectly fell back to stdout.'
}
Assert-FailureCode `
    -Name 'multiple stage markers on one line fail closed' `
    -Code 'stagemem-format' `
    -Native ('stagemem: q=TRUNCATED ' + $healthyStageNominalText)
Assert-FailureCode `
    -Name 'bare CR cannot hide truncated stage marker' `
    -Code 'stagemem-format' `
    -Native ('stagemem: q=TRUNCATED' + "`r" +
        $healthyStageNominalText)
Assert-FailureCode -Name 'malformed lower stage source is still checked' `
    -Code 'stagemem-format' -Native $healthyStageNominalText `
    -Stdout 'stagemem: q=1'
[void](Assert-Healthy `
    -Name 'lower stage q and lifecycle are non-authoritative' `
    -Native $healthyStageNominalText `
    -Stdout $healthyStageEarlyFailure[0].Replace(
        'q=1 lq/rq=0/1', 'q=9 lq/rq=9/9'))
Assert-FailureCode -Name 'unknown lower stage event is still checked' `
    -Code 'stagemem-lifecycle' -Native $healthyStageNominalText `
    -Stdout $healthyStageEarlyFailure[0].Replace(
        'e=room-load-begin', 'e=unknown')

$stageSimpleCases = @(
    [pscustomobject]@{
        Name = 'stage uint32 overflow'; Code = 'stagemem-format'
        Stdout = $healthyStageEarlyFailure[0].Replace(
            'q=1 ', 'q=100000000 ')
    },
    [pscustomobject]@{
        Name = 'stage uppercase hex'; Code = 'stagemem-format'
        Stdout = $healthyStageEarlyFailure[0].Replace(
            'mi=1000/', 'mi=A000/')
    },
    [pscustomobject]@{
        Name = 'stage context invalid'; Code = 'stagemem-context'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'ctx_valid=1', 'ctx_valid=0')
    },
    [pscustomobject]@{
        Name = 'stage aggregate invalid'; Code = 'stagemem-invalid'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=0/0/0')
    },
    [pscustomobject]@{
        Name = 'stage terminal'; Code = 'stagemem-terminal'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=1/1/0')
    },
    [pscustomobject]@{
        Name = 'stage saturated'; Code = 'stagemem-saturated'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=1/0/1')
    },
    [pscustomobject]@{
        Name = 'stage q gap'; Code = 'stagemem-sequence'
        Stdout = @($healthyStageEarlyFailure[0],
            $healthyStageEarlyFailure[1].Replace(
                'q=2 ', 'q=3 ')) -join "`n"
    },
    [pscustomobject]@{
        Name = 'stage duplicate q'; Code = 'stagemem-sequence'
        Stdout = @($healthyStageEarlyFailure[0],
            $healthyStageEarlyFailure[1].Replace(
                'q=2 ', 'q=1 ')) -join "`n"
    },
    [pscustomobject]@{
        Name = 'stage lq mismatch'; Code = 'stagemem-lifecycle'
        Stdout = $healthyStageNominalText.Replace(
            'q=1 lq/rq=1/0', 'q=1 lq/rq=2/0')
    },
    [pscustomobject]@{
        Name = 'stage rq mismatch'; Code = 'stagemem-lifecycle'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'q=2 lq/rq=0/1', 'q=2 lq/rq=0/2')
    },
    [pscustomobject]@{
        Name = 'stage non-end result'; Code = 'stagemem-lifecycle'
        Stdout = $healthyStageEarlyFailure[0].Replace(
            'ok=ffffffff', 'ok=0')
    },
    [pscustomobject]@{
        Name = 'stage non-boolean result'; Code = 'stagemem-lifecycle'
        Stdout = $healthyStageEarlyFailureText.Replace('ok=0', 'ok=2')
    },
    [pscustomobject]@{
        Name = 'stage room context drift'; Code = 'stagemem-lifecycle'
        Stdout = @($healthyStageEarlyFailure[0],
            $healthyStageEarlyFailure[1].Replace(
                'rs=7 mode=2', 'rs=8 mode=2')) -join "`n"
    },
    [pscustomobject]@{
        Name = 'stage active outside boolean domain'
        Code = 'stagemem-lifecycle'
        Stdout = $healthyStageEarlyFailureText.Replace(
            'active=0', 'active=2')
    },
    [pscustomobject]@{
        Name = 'stage success without unload'; Code = 'stagemem-lifecycle'
        Stdout = $healthyStageEarlyFailureText.Replace('ok=0', 'ok=1')
    },
    [pscustomobject]@{
        Name = 'stage after-unload without load'; Code = 'stagemem-lifecycle'
        Stdout = New-StageMemoryRecord '1' '0' '0' `
            'room-after-unload' '0' 'ffffffff' 'ffffffff' `
            'ffffffff' 'ffffffff' 'ffffffff'
    },
    [pscustomobject]@{
        Name = 'stage open room at EOF'; Code = 'stagemem-pairing'
        Stdout = $healthyStageEarlyFailure[0]
    },
    [pscustomobject]@{
        Name = 'stage open level at EOF'; Code = 'stagemem-pairing'
        Stdout = $healthyStageNominal[0]
    }
)
foreach ($case in $stageSimpleCases) {
    Assert-FailureCode -Name $case.Name -Code $case.Code `
        -Stdout $case.Stdout
}

function New-FloorLifeRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Q,
        [Parameter(Mandatory = $true)][string]$Event,
        [Parameter(Mandatory = $true)][string]$Epoch,
        [Parameter(Mandatory = $true)][string]$Bootstraps,
        [Parameter(Mandatory = $true)][string]$Rollovers,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][string]$Counters
    )
    return 'floorlife: q=' + $Q + ' e=' + $Event +
        ' epoch=' + $Epoch + ' boot/roll=' + $Bootstraps + '/' +
        $Rollovers + ' phase=' + $Phase + ' ' + $Counters
}

$floorUnscoped =
    'heap=9/9/0/0,100/100/0/0 hphase=0/0/0,0/0/0 ' +
    'slab=11/11/0/0 sphase=0/0/0 valid/term/sat=1/0/0'
$floorLevelScoped =
    'heap=9/7/0/2,120/100/0/20 hphase=2/0/0,20/0/0 ' +
    'slab=11/10/0/1 sphase=1/0/0 valid/term/sat=1/0/0'
$floorRoomScoped =
    'heap=9/7/0/2,120/100/0/20 hphase=0/2/0,0/20/0 ' +
    'slab=11/10/0/1 sphase=0/1/0 valid/term/sat=1/0/0'
$floorPriorAfterRollover =
    'heap=9/7/2/0,120/100/20/0 hphase=0/0/0,0/0/0 ' +
    'slab=11/10/1/0 sphase=0/0/0 valid/term/sat=1/0/0'

$healthyFloorNominal = @(
    (New-FloorLifeRecord '1' 'level-begin' '1' '1' '0' `
        'level-init' $floorUnscoped),
    (New-FloorLifeRecord '2' 'room-reuse' '1' '1' '0' `
        'level-init' $floorLevelScoped),
    (New-FloorLifeRecord '3' 'room-load-begin' '1' '1' '0' `
        'room-load' $floorLevelScoped),
    (New-FloorLifeRecord '4' 'room-after-unload' '1' '1' '0' `
        'room-load' $floorLevelScoped),
    (New-FloorLifeRecord '5' 'room-load-end' '1' '1' '0' `
        'level-init' $floorLevelScoped),
    (New-FloorLifeRecord '6' 'level-end' '1' '1' '0' `
        'play' $floorLevelScoped)
)
$healthyFloorNominalText = $healthyFloorNominal -join "`n"
$healthyStageAndFloorNominal =
    @($healthyStageNominalText, $healthyFloorNominalText) -join "`n"

$floorNominalResult = Assert-Healthy `
    -Name 'floor lifetime nominal lifecycle' `
    -Stdout $healthyStageAndFloorNominal `
    -FloorLifeRequired $true
if (-not $floorNominalResult.FloorLifeRequired -or
        $floorNominalResult.FloorLifeAllSourceRecords -ne 6 -or
        -not $floorNominalResult.FloorLifePresent -or
        $floorNominalResult.FloorLifeAuthoritativeSource -ne 'stdout' -or
        $floorNominalResult.FloorLifeAuthoritativeRecords -ne 6 -or
        $floorNominalResult.FloorLifeEpoch -ne 1 -or
        $floorNominalResult.FloorLifeBootstraps -ne 1 -or
        $floorNominalResult.FloorLifeRollovers -ne 0 -or
        $floorNominalResult.FloorLifePhase -ne 'play') {
    throw 'Healthy nominal floor-lifetime counters were not preserved.'
}
$floorImpossiblePlayBirths = @($healthyFloorNominal)
$floorImpossiblePlayBirths[1] = New-FloorLifeRecord '2' 'room-reuse' `
    '1' '1' '0' 'level-init' `
    ('heap=9/7/0/2,120/100/0/20 hphase=0/0/2,0/0/20 ' +
     'slab=11/10/0/1 sphase=0/0/1 valid/term/sat=1/0/0')
Assert-FailureCode `
    -Name 'floor birth phase must have been entered in current epoch' `
    -Code 'floorlife-lifecycle' `
    -Stdout (@($healthyStageNominalText,
        ($floorImpossiblePlayBirths -join "`n")) -join "`n")

$stageUnscopedGrowth = @($healthyStageNominal)
$stageUnscopedGrowth[1] = $stageUnscopedGrowth[1].Replace(
    'ledger=9 ', 'ledger=a ').Replace(
    'slab=2/2/0/11/14/3', 'slab=2/2/0/12/15/3')
$floorUnscopedGrowth = @($healthyFloorNominal)
$floorUnscopedGrowth[1] = New-FloorLifeRecord '2' 'room-reuse' `
    '1' '1' '0' 'level-init' `
    ('heap=a/a/0/0,100/100/0/0 hphase=0/0/0,0/0/0 ' +
     'slab=12/12/0/0 sphase=0/0/0 valid/term/sat=1/0/0')
Assert-FailureCode `
    -Name 'combined unscoped population cannot grow after bootstrap' `
    -Code 'floorlife-lifecycle' `
    -Stdout (@(($stageUnscopedGrowth -join "`n"),
        ($floorUnscopedGrowth -join "`n")) -join "`n")

Assert-FailureCode -Name 'required floor lifetime is absent' `
    -Code 'floorlife-missing' -Stdout $healthyStageNominalText `
    -FloorLifeRequired $true
$floorImpliesHealth = Invoke-HealthFixture -Required $false `
    -FloorLifeRequired $true
if ($floorImpliesHealth.Passed -or -not $floorImpliesHealth.Required -or
        $floorImpliesHealth.FailureCodes -notcontains 'floorlife-missing') {
    throw 'Floor-lifetime requirement did not imply runtime-health parsing.'
}

$floorReordered = @(
    $healthyFloorNominal[5], $healthyStageNominal[5],
    $healthyFloorNominal[3], $healthyStageNominal[3],
    $healthyFloorNominal[1], $healthyStageNominal[1],
    $healthyFloorNominal[4], $healthyStageNominal[4],
    $healthyFloorNominal[0], $healthyStageNominal[0],
    $healthyFloorNominal[2], $healthyStageNominal[2]
) -join "`n"
$floorReorderedResult = Assert-Healthy `
    -Name 'floor lifetime q order overrides physical order' `
    -Native $floorReordered
if ($floorReorderedResult.FloorLifeAuthoritativeSource -ne 'native' -or
        $floorReorderedResult.FloorLifeAuthoritativeRecords -ne 6 -or
        $floorReorderedResult.FloorLifePhase -ne 'play') {
    throw 'Reordered floor-lifetime records were not reconstructed by q.'
}
$floorGameResult = Assert-Healthy `
    -Name 'floor lifetime game-log fallback authority' `
    -Game $healthyStageAndFloorNominal
if ($floorGameResult.StageMemoryAuthoritativeSource -ne 'game' -or
        $floorGameResult.FloorLifeAuthoritativeSource -ne 'game' -or
        $floorGameResult.FloorLifeAuthoritativeRecords -ne 6) {
    throw 'Game-log floor-lifetime fallback authority was not selected.'
}

$floorRaceResult = Assert-Healthy `
    -Name 'floor lifetime pre-stop stdout ends before native' `
    -Stdout (@($healthyStageNominal[0..3] +
        $healthyFloorNominal[0..2]) -join "`n") `
    -Native $healthyStageAndFloorNominal
if ($floorRaceResult.FloorLifeAllSourceRecords -ne 9 -or
        $floorRaceResult.FloorLifeAuthoritativeSource -ne 'native' -or
        $floorRaceResult.FloorLifeAuthoritativeRecords -ne 6) {
    throw 'Native floor-lifetime authority did not tolerate a lower prefix.'
}

$healthyFloorContinue = @(
    (New-FloorLifeRecord '1' 'room-load-begin' '1' '1' '0' `
        'room-load' $floorUnscoped),
    (New-FloorLifeRecord '2' 'room-load-end' '1' '1' '0' `
        'play' $floorRoomScoped)
)
$floorContinueResult = Assert-Healthy `
    -Name 'floor lifetime Continue bootstrap' `
    -Stdout (@($healthyStageEarlyFailure + $healthyFloorContinue) -join "`n")
if ($floorContinueResult.FloorLifeEpoch -ne 1 -or
        $floorContinueResult.FloorLifeBootstraps -ne 1 -or
        $floorContinueResult.FloorLifeRollovers -ne 0 -or
        $floorContinueResult.FloorLifePhase -ne 'play') {
    throw 'Continue room-load bootstrap was not reconstructed.'
}
$floorContinueWithCurrentAtBootstrap = @(
    (New-FloorLifeRecord '1' 'room-load-begin' '1' '1' '0' `
        'room-load' $floorRoomScoped),
    $healthyFloorContinue[1]
)
Assert-FailureCode `
    -Name 'Continue bootstrap cannot create current births in same record' `
    -Code 'floorlife-lifecycle' `
    -Stdout (@($healthyStageEarlyFailure +
        $floorContinueWithCurrentAtBootstrap) -join "`n")

$healthyStageSecondFloor = @(
    (New-StageMemoryRecord '7' '7' '0' 'level-begin' '1' `
        '5' '2' 'ffffffff' 'ffffffff' 'ffffffff'),
    (New-StageMemoryRecord '8' '7' '0' 'level-end' '1' `
        '5' '2' 'ffffffff' 'ffffffff' 'ffffffff')
)
$healthyFloorSecondFloor = @(
    (New-FloorLifeRecord '7' 'level-begin' '2' '1' '1' `
        'level-init' $floorPriorAfterRollover),
    (New-FloorLifeRecord '8' 'level-end' '2' '1' '1' `
        'play' $floorPriorAfterRollover)
)
$healthyStageTwoFloors =
    @($healthyStageNominal + $healthyStageSecondFloor) -join "`n"
$healthyFloorTwoFloors =
    @($healthyFloorNominal + $healthyFloorSecondFloor) -join "`n"
$floorRolloverResult = Assert-Healthy `
    -Name 'floor lifetime second-level rollover' `
    -Stdout (@($healthyStageTwoFloors, $healthyFloorTwoFloors) -join "`n")
if ($floorRolloverResult.FloorLifeEpoch -ne 2 -or
        $floorRolloverResult.FloorLifeBootstraps -ne 1 -or
        $floorRolloverResult.FloorLifeRollovers -ne 1 -or
        $floorRolloverResult.FloorLifePhase -ne 'play') {
    throw 'Second-level floor rollover was not reconstructed.'
}
$floorPriorGrowth = @($healthyFloorNominal + $healthyFloorSecondFloor)
$floorPriorGrowth[7] = New-FloorLifeRecord '8' 'level-end' '2' '1' '1' `
    'play' `
    ('heap=9/6/3/0,120/f0/30/0 hphase=0/0/0,0/0/0 ' +
     'slab=11/10/1/0 sphase=0/0/0 valid/term/sat=1/0/0')
Assert-FailureCode `
    -Name 'combined prior population cannot grow inside one epoch' `
    -Code 'floorlife-lifecycle' `
    -Stdout (@($healthyStageTwoFloors,
        ($floorPriorGrowth -join "`n")) -join "`n")

$floorTruncatedNative = Assert-FailureCode `
    -Name 'truncated native floor marker does not fall back' `
    -Code 'floorlife-format' `
    -Stdout $healthyStageAndFloorNominal `
    -Native (@($healthyStageNominalText, 'floorlife: q=1') -join "`n") `
    -PassThru
if ($floorTruncatedNative.FloorLifeAuthoritativeSource -ne 'native' -or
        $floorTruncatedNative.FloorLifeAuthoritativeRecords -ne 1) {
    throw 'Truncated native floor marker incorrectly fell back to stdout.'
}
$floorUppercaseNative = Assert-FailureCode `
    -Name 'uppercase native floor marker does not fall back' `
    -Code 'floorlife-format' `
    -Stdout $healthyStageAndFloorNominal `
    -Native (@($healthyStageNominalText,
        $healthyFloorNominal[0].Replace('floorlife:', 'FLOORLIFE:')) `
        -join "`n") -PassThru
if ($floorUppercaseNative.FloorLifeAuthoritativeSource -ne 'native' -or
        $floorUppercaseNative.FloorLifeAuthoritativeRecords -ne 1) {
    throw 'Uppercase native floor marker incorrectly fell back to stdout.'
}
Assert-FailureCode `
    -Name 'multiple floor markers on one line fail closed' `
    -Code 'floorlife-format' `
    -Native (@($healthyStageNominalText,
        ('floorlife: q=TRUNCATED ' + $healthyFloorNominalText)) -join "`n")
Assert-FailureCode `
    -Name 'bare CR cannot hide truncated floor marker' `
    -Code 'floorlife-format' `
    -Native (@($healthyStageNominalText,
        ('floorlife: q=TRUNCATED' + "`r" + $healthyFloorNominalText)) `
        -join "`n")
Assert-FailureCode `
    -Name 'malformed lower floor source is still checked' `
    -Code 'floorlife-format' `
    -Native $healthyStageAndFloorNominal `
    -Stdout 'floorlife: q=1'
[void](Assert-Healthy `
    -Name 'lower floor q is non-authoritative' `
    -Native $healthyStageAndFloorNominal `
    -Stdout $healthyFloorNominal[0].Replace('q=1 ', 'q=9 '))
Assert-FailureCode `
    -Name 'lower floor valid record cannot precede bootstrap' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout $healthyFloorNominal[0].Replace(
        'epoch=1 boot/roll=1/0', 'epoch=0 boot/roll=0/0')
Assert-FailureCode `
    -Name 'lower floor epoch cannot outrun event sequence' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout $healthyFloorNominal[0].Replace(
        'epoch=1 boot/roll=1/0', 'epoch=2 boot/roll=1/1')
Assert-FailureCode `
    -Name 'lower floor fixed event phase must agree' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout $healthyFloorNominal[0].Replace(
        'phase=level-init', 'phase=play')
Assert-FailureCode `
    -Name 'lower floor level begin current buckets must be empty' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout (New-FloorLifeRecord '1' 'level-begin' '1' '1' '0' `
        'level-init' $floorLevelScoped)
Assert-FailureCode `
    -Name 'lower floor reuse cannot occur in room load phase' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout $healthyFloorNominal[1].Replace(
        'phase=level-init', 'phase=room-load')
Assert-FailureCode `
    -Name 'lower floor prior cannot exist before rollover' `
    -Code 'floorlife-lifecycle' `
    -Native $healthyStageAndFloorNominal `
    -Stdout (New-FloorLifeRecord '2' 'room-reuse' '1' '1' '0' `
        'level-init' `
        ('heap=9/7/1/1,120/100/10/10 hphase=1/0/0,10/0/0 ' +
         'slab=11/10/1/0 sphase=0/0/0 valid/term/sat=1/0/0'))
Assert-FailureCode `
    -Name 'lower floor zero-count bucket cannot carry bytes' `
    -Code 'floorlife-accounting' `
    -Native $healthyStageAndFloorNominal `
    -Stdout (New-FloorLifeRecord '2' 'room-reuse' '1' '1' '0' `
        'level-init' `
        ('heap=9/9/0/0,110/100/0/10 hphase=0/0/0,10/0/0 ' +
         'slab=11/11/0/0 sphase=0/0/0 valid/term/sat=1/0/0'))
Assert-FailureCode `
    -Name 'native stage cannot pair with lower stdout floor' `
    -Code 'floorlife-pairing' `
    -Native $healthyStageNominalText `
    -Stdout $healthyFloorNominalText
Assert-FailureCode `
    -Name 'native floor cannot pair with lower stdout stage' `
    -Code 'floorlife-pairing' `
    -Native $healthyFloorNominalText `
    -Stdout $healthyStageNominalText

$floorSimpleCases = @(
    [pscustomobject]@{
        Name = 'floor uint32 overflow'; Code = 'floorlife-format'
        Floor = $healthyFloorNominalText.Replace(
            'epoch=1 ', 'epoch=100000000 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor uppercase hex'; Code = 'floorlife-format'
        Floor = $healthyFloorNominalText.Replace(
            'heap=9/', 'heap=A/')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor unknown event'; Code = 'floorlife-event'
        Floor = $healthyFloorNominalText.Replace(
            'e=room-reuse', 'e=unknown')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor unknown phase'; Code = 'floorlife-phase'
        Floor = $healthyFloorNominalText.Replace(
            'phase=room-load', 'phase=unknown')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor pre-floor phase'; Code = 'floorlife-phase'
        Floor = $healthyFloorNominalText.Replace(
            'phase=level-init', 'phase=pre-floor')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor invalid'; Code = 'floorlife-invalid'
        Floor = $healthyFloorNominalText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=0/0/0')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor terminal'; Code = 'floorlife-terminal'
        Floor = $healthyFloorNominalText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=1/1/0')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor saturated'; Code = 'floorlife-saturated'
        Floor = $healthyFloorNominalText.Replace(
            'valid/term/sat=1/0/0', 'valid/term/sat=1/0/1')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor heap age accounting'; Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            'heap=9/7/0/2,', 'heap=a/7/0/2,')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor heap byte accounting'; Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            ',120/100/0/20 ', ',121/100/0/20 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor heap phase accounting'; Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            'hphase=2/0/0,', 'hphase=1/0/0,')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor heap phase-byte accounting'
        Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            ',20/0/0 slab=', ',10/0/0 slab=')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor slab age accounting'; Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            'slab=11/10/0/1 ', 'slab=12/10/0/1 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor slab phase accounting'; Code = 'floorlife-accounting'
        Floor = $healthyFloorNominalText.Replace(
            'sphase=1/0/0 ', 'sphase=0/0/0 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor q gap'; Code = 'floorlife-sequence'
        Floor = $healthyFloorNominalText.Replace(
            'floorlife: q=5 ', 'floorlife: q=7 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor duplicate q'; Code = 'floorlife-sequence'
        Floor = $healthyFloorNominalText.Replace(
            'floorlife: q=5 ', 'floorlife: q=4 ')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor missing paired q'; Code = 'floorlife-pairing'
        Floor = $healthyFloorNominal[0..4] -join "`n"
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor event differs from stage'; Code = 'floorlife-pairing'
        Floor = $healthyFloorNominalText.Replace(
            'e=room-reuse', 'e=room-load-begin')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor has no stage stream'; Code = 'floorlife-pairing'
        Floor = $healthyFloorNominalText
        Stage = ''
    },
    [pscustomobject]@{
        Name = 'floor first-event boot counter'; Code = 'floorlife-lifecycle'
        Floor = $healthyFloorNominalText.Replace(
            'q=1 e=level-begin epoch=1 boot/roll=1/0',
            'q=1 e=level-begin epoch=1 boot/roll=0/1')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor roll changes on room event'; Code = 'floorlife-lifecycle'
        Floor = $healthyFloorNominalText.Replace(
            'q=2 e=room-reuse epoch=1 boot/roll=1/0',
            'q=2 e=room-reuse epoch=2 boot/roll=1/1')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor known phase transition drift'
        Code = 'floorlife-lifecycle'
        Floor = $healthyFloorNominalText.Replace(
            'q=3 e=room-load-begin epoch=1 boot/roll=1/0 phase=room-load',
            'q=3 e=room-load-begin epoch=1 boot/roll=1/0 phase=play')
        Stage = $healthyStageNominalText
    },
    [pscustomobject]@{
        Name = 'floor rollover keeps current allocations'
        Code = 'floorlife-lifecycle'
        Floor = $healthyFloorTwoFloors.Replace(
            'q=7 e=level-begin epoch=2 boot/roll=1/1 phase=level-init ' +
            $floorPriorAfterRollover,
            'q=7 e=level-begin epoch=2 boot/roll=1/1 phase=level-init ' +
            $floorLevelScoped)
        Stage = $healthyStageTwoFloors
    },
    [pscustomobject]@{
        Name = 'floor room load open at EOF'; Code = 'floorlife-pairing'
        Floor = $healthyFloorContinue[0]
        Stage = $healthyStageEarlyFailure[0]
    }
)
foreach ($case in $floorSimpleCases) {
    Assert-FailureCode -Name $case.Name -Code $case.Code `
        -Stdout (@($case.Stage, $case.Floor) -join "`n")
}

$floorHeapCrossDomain = $healthyFloorNominalText.Replace(
    'heap=9/9/0/0,', 'heap=a/a/0/0,').Replace(
    'heap=9/7/0/2,', 'heap=a/8/0/2,')
Assert-FailureCode -Name 'floor heap total differs from stage ledger' `
    -Code 'floorlife-cross-domain' `
    -Stdout (@($healthyStageNominalText, $floorHeapCrossDomain) -join "`n")
$floorSlabCrossDomain = $healthyFloorNominalText.Replace(
    'slab=11/11/0/0 ', 'slab=12/12/0/0 ').Replace(
    'slab=11/10/0/1 ', 'slab=12/11/0/1 ')
Assert-FailureCode -Name 'floor slab total differs from stage live slots' `
    -Code 'floorlife-cross-domain' `
    -Stdout (@($healthyStageNominalText, $floorSlabCrossDomain) -join "`n")
Assert-FailureCode -Name 'valid floor record requires valid stage record' `
    -Code 'floorlife-cross-domain' `
    -Stdout (@($healthyStageNominalText.Replace(
        'valid/term/sat=1/0/0', 'valid/term/sat=0/0/0'),
        $healthyFloorNominalText) -join "`n")

$stageReuseFirst = New-StageMemoryRecord '1' '0' '1' 'room-reuse' '0' `
    'ffffffff' 'ffffffff' '7' '2' 'ffffffff'
$floorReuseFirst = New-FloorLifeRecord '1' 'room-reuse' '1' '1' '0' `
    'play' $floorUnscoped
Assert-FailureCode -Name 'floor bootstrap cannot start at room reuse' `
    -Code 'floorlife-lifecycle' `
    -Stdout (@($stageReuseFirst, $floorReuseFirst) -join "`n")

function Assert-ExactFileHash {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Saved runtime-health evidence is missing: $Path"
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $ExpectedSha256.ToUpperInvariant()) {
        throw "Saved runtime-health evidence hash drift: $Path ($actual)"
    }
}

function Assert-TsvInventoryEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Inventory,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $matches = @(Get-Content -LiteralPath $Inventory | Where-Object {
        $_.StartsWith($RelativePath + "`t", [StringComparison]::Ordinal)
    })
    if ($matches.Count -ne 1) {
        throw "Expected one TSV inventory entry for $RelativePath."
    }
    $fields = @($matches[0] -split "`t")
    if ($fields.Count -ne 3 -or $fields[0] -ne $RelativePath) {
        throw "Malformed TSV inventory entry for $RelativePath."
    }
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Inventoried evidence is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ([string]$item.Length -ne $fields[1] -or
            $actualHash -ne $fields[2].ToUpperInvariant()) {
        throw "Inventoried evidence drift: $RelativePath"
    }
    return $path
}

function Assert-SpaceInventoryEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Inventory,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $matches = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content -LiteralPath $Inventory) {
        $shape = [regex]::Match(
            $line, '\A([0-9A-F]{64}) ([0-9]+) (.+)\z')
        if ($shape.Success -and $shape.Groups[3].Value -eq $RelativePath) {
            [void]$matches.Add($shape)
        }
    }
    if ($matches.Count -ne 1) {
        throw "Expected one space inventory entry for $RelativePath."
    }
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Inventoried evidence is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ([string]$item.Length -ne $matches[0].Groups[2].Value -or
            $actualHash -ne $matches[0].Groups[1].Value) {
        throw "Inventoried evidence drift: $RelativePath"
    }
    return $path
}

# Re-run the classifier over the immutable real Stage2->Stage3 and Hybrid192
# evidence after first rehashing every consumed source against its own sealed
# inventory/receipt.  Both predate floorlife and must remain valid when the new
# domain is not explicitly required.
$repoRoot = Split-Path -Parent $PSScriptRoot
$stageEvidenceRoot = Join-Path $repoRoot `
    'recomp\out\vita\stage2-trapdoor-vita-d82565a-20260825-110451'
$stageInventory = Join-Path $stageEvidenceRoot 'complete-inventory.tsv'
$stageSeal = Join-Path $stageEvidenceRoot 'seal-receipt.json'
Assert-ExactFileHash -Path $stageInventory -ExpectedSha256 `
    'EE64FA0AB2891465FE0ACD0E29E48EEADA6146A9857EA661A111E9EA9FA838D1'
Assert-ExactFileHash -Path $stageSeal -ExpectedSha256 `
    '6DF89EB4329A1CF843C4CD148F4A317CE80A0DB37BA2275A75948A80649DA578'
$stageSealObject = Get-Content -LiteralPath $stageSeal -Raw |
    ConvertFrom-Json
if ($stageSealObject.contract -ne 'STAGE2_TRAPDOOR_VITA_EVIDENCE_SEAL_V1' -or
        $stageSealObject.result -ne 'PASS' -or
        $stageSealObject.vita3k_processes -ne 0) {
    throw 'Stage2->Stage3 evidence seal is not a clean PASS receipt.'
}
$stageStdoutPath = Assert-TsvInventoryEntry -Root $stageEvidenceRoot `
    -Inventory $stageInventory -RelativePath 'run\vita3k.pre-stop.log'
$stageNativePath = Assert-TsvInventoryEntry -Root $stageEvidenceRoot `
    -Inventory $stageInventory `
    -RelativePath 'run\first-arm-fault.current.log'
$stageGamePath = Assert-TsvInventoryEntry -Root $stageEvidenceRoot `
    -Inventory $stageInventory -RelativePath 'run\game-log.current.txt'
$stageEvidenceResult = Assert-Healthy `
    -Name 'sealed real Stage2 to Stage3 legacy evidence' `
    -Stdout ([IO.File]::ReadAllText($stageStdoutPath)) `
    -Native ([IO.File]::ReadAllText($stageNativePath)) `
    -Game ([IO.File]::ReadAllText($stageGamePath))
if ($stageEvidenceResult.StageMemoryAuthoritativeSource -ne 'native' -or
        $stageEvidenceResult.StageMemoryAuthoritativeRecords -ne 15 -or
        $stageEvidenceResult.FloorLifeAllSourceRecords -ne 0 -or
        $stageEvidenceResult.FloorLifeAuthoritativeSource -ne 'none') {
    throw 'Sealed Stage2->Stage3 legacy evidence counters drifted.'
}
Assert-FailureCode -Name 'sealed Stage3 legacy evidence requires new domain' `
    -Code 'floorlife-missing' `
    -Stdout ([IO.File]::ReadAllText($stageStdoutPath)) `
    -Native ([IO.File]::ReadAllText($stageNativePath)) `
    -Game ([IO.File]::ReadAllText($stageGamePath)) `
    -FloorLifeRequired $true

$hybridEvidenceRoot = Join-Path $repoRoot `
    'recomp\out\vita\room-entry-hybrid-eda3a47-isolated-auto-20260825-0627'
$hybridInventory = Join-Path $hybridEvidenceRoot 'evidence-inventory.txt'
$hybridReceipt = Join-Path $hybridEvidenceRoot 'isolation-receipt.txt'
Assert-ExactFileHash -Path $hybridInventory -ExpectedSha256 `
    '3396D2917CF2F43672EAB1E6D9AE204471AA73AF4702573B6C5BDA4896D7E952'
Assert-ExactFileHash -Path $hybridReceipt -ExpectedSha256 `
    'B06913B3EB8CD425B0BB31F68F51133B9FC9CD2FB84EBDC21028D9BD3BC8C8DC'
$hybridReceiptText = [IO.File]::ReadAllText($hybridReceipt)
if ($hybridReceiptText -notmatch '(?m)^result=PASS\r?$' -or
        $hybridReceiptText -notmatch
        '(?m)^contract=ROOM_ENTRY_HYBRID_ISOLATED_AUTOGAME_V1\r?$' -or
        $hybridReceiptText -notmatch
        '(?m)^native_log_sha256=B63919B32C3A9F11929814115BE9960BA2BB311EAE69A7090A7D7D8DC3B65393\r?$') {
    throw 'Hybrid192 evidence receipt is not the expected clean PASS.'
}
$hybridStdoutPath = Assert-SpaceInventoryEntry -Root $hybridEvidenceRoot `
    -Inventory $hybridInventory -RelativePath 'vita3k.pre-stop.log'
$hybridNativePath = Assert-SpaceInventoryEntry -Root $hybridEvidenceRoot `
    -Inventory $hybridInventory -RelativePath 'first-arm-fault.current.log'
$hybridGamePath = Assert-SpaceInventoryEntry -Root $hybridEvidenceRoot `
    -Inventory $hybridInventory -RelativePath 'game-log.current.txt'
$hybridEvidenceResult = Assert-Healthy `
    -Name 'sealed real Hybrid192 legacy evidence' `
    -Stdout ([IO.File]::ReadAllText($hybridStdoutPath)) `
    -Native ([IO.File]::ReadAllText($hybridNativePath)) `
    -Game ([IO.File]::ReadAllText($hybridGamePath))
if ($hybridEvidenceResult.StageMemoryAllSourceRecords -ne 0 -or
        $hybridEvidenceResult.FloorLifeAllSourceRecords -ne 0 -or
        $hybridEvidenceResult.FloorLifeAuthoritativeSource -ne 'none') {
    throw 'Sealed Hybrid192 legacy evidence counters drifted.'
}

$healthyPool =
    'KAGE VITA OPENAL POOL: phase=init state=2 init=0 backing=12410880 ' +
    'live=0 bytes=0 peak=0/0 ops=0/0/0 pool_fail=0 stranded=0 corrupt=0 pre=0'
$healthyAudio =
    'KAGE VITA AUDIO INIT: status=ready stage=complete ' +
    'device=0x89c0e470 context=0x89c17f80 al=0x00000000 ' +
    'manager_sources=64 device_sources=80 stream_headroom=16 buffers=64 ' +
    'pump=main-loop+room-cooperative guest_thread=off'
$healthyHeap =
    'heapovf: e=first-use q=1 op=1 req=12 live=1/12 peak=1/12 ' +
    'a/f/r=1/0/0 n2p/p2n=0/0 nf/pf=0/0 cons=0 ' +
    'raw/str/int=1/0/1/65536 state=1 term=0 valid/sat=1/0'
$rawSlab =
    'roomslab: e=page-power2 pages=2(2+0) live/move/peak=4032/0/4032 ' +
    'a/f/r/ma/mc=4032/0/0/0/0 fb=0 known=0/0 unexpected=0 reject=0 ' +
    'raw=2/131072 retry=0/0 term/sat=0/0'
$rawSlabExternal =
    'roomslabx: e=page-power2 chunks=0 alloc/oom/post=0/0/0 rb=0/0 ' +
    'reset=0/0 xsat=0 bytes=req/use/ret=0/0/0 orphan=-1/0/0 ' +
    'state/fault/sys/result=1/0/0/0'
$externalFirstSlab =
    'roomslab: e=external-first pages=97(96+1) ' +
    'live/move/peak=387073/0/387073 ' +
    'a/f/r/ma/mc=387073/0/0/0/0 fb=0 known=0/0 unexpected=0 reject=0 ' +
    'raw=96/6291456 retry=0/0 term/sat=0/0'
$externalFirstState =
    'roomslabx: e=external-first chunks=1 alloc/oom/post=1/0/0 rb=0/0 ' +
    'reset=0/0 xsat=0 bytes=req/use/ret=1052672/1048576/1056768 ' +
    'orphan=-1/0/0 state/fault/sys/result=1/0/1/10'
$externalOomSlab =
    'roomslab: e=external-oom-power2 pages=112(96+16) ' +
    'live/move/peak=451584/0/451584 ' +
    'a/f/r/ma/mc=451584/0/0/0/0 fb=1 known=0/0 unexpected=0 reject=0 ' +
    'raw=96/6291456 retry=4032/0 term/sat=0/0'
$externalOomState =
    'roomslabx: e=external-oom-power2 chunks=1 alloc/oom/post=2/1/0 ' +
    'rb=0/0 reset=0/0 xsat=0 ' +
    'bytes=req/use/ret=1052672/1048576/1056768 orphan=-1/0/0 ' +
    'state/fault/sys/result=1/0/1/-2147352575'
$fallbackSlab = $externalOomSlab.Replace(
    'e=external-oom-power2', 'e=fallback')
$fallbackState = $externalOomState.Replace(
    'e=external-oom-power2', 'e=fallback')

$rawResult = Assert-Healthy -Name 'raw slab' `
    -Stdout (@($healthyPool, $healthyAudio, $healthyHeap,
        $rawSlab, $rawSlabExternal) -join "`n")
if ($rawResult.RoomSlabRecords -ne 1 -or
        $rawResult.RoomSlabExternalRecords -ne 1 -or
        $rawResult.HeapOverflowRecords -ne 1 -or
        $rawResult.OpenALPoolRecords -ne 1 -or
        $rawResult.StageMemoryAllSourceRecords -ne 0 -or
        $rawResult.StageMemoryAuthoritativeSource -ne 'none') {
    throw 'Healthy raw telemetry record counts were not preserved.'
}
[void](Assert-Healthy -Name 'external-first slab' `
    -Native (@($externalFirstSlab, $externalFirstState) -join "`n"))
[void](Assert-Healthy -Name 'external OOM plus fallback' `
    -Stdout (@($externalOomSlab, $externalOomState,
        $fallbackSlab, $fallbackState) -join "`n"))
[void](Assert-Healthy -Name 'paired slab with interleaved unrelated log' `
    -Stdout (@($rawSlab, 'unrelated logger record',
        $rawSlabExternal) -join "`n"))
[void](Assert-Healthy -Name 'pre-stop stdout ends between pair' `
    -Stdout (@($rawSlab, $rawSlabExternal, $externalFirstSlab) -join "`n") `
    -Native (@($rawSlab, $rawSlabExternal,
        $externalFirstSlab, $externalFirstState) -join "`n"))

Assert-FailureCode -Name 'missing later external pair' `
    -Code 'roomslab-pairing' `
    -Stdout (@($rawSlab, $rawSlabExternal, $externalFirstSlab) -join "`n")
Assert-FailureCode -Name 'wrong external edge pair' `
    -Code 'roomslab-pairing' `
    -Stdout (@($rawSlab, $externalFirstState) -join "`n")

$simpleCases = @(
    [pscustomobject]@{
        Name = 'truncated heap telemetry'; Code = 'heapovf-format'
        Stdout = 'heapovf: e=first-use'; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'truncated slab telemetry'; Code = 'roomslab-format'
        Stdout = 'roomslab: e=page-power2'; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'truncated external telemetry'; Code = 'roomslabx-format'
        Stdout = 'roomslabx: e=external-first'; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'truncated OpenAL pool telemetry'; Code = 'openal-pool-format'
        Stdout = 'KAGE VITA OPENAL POOL: phase=init state=2'
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'truncated OpenAL audio telemetry'; Code = 'openal-audio-format'
        Stdout = $healthyAudio.Replace('stage=complete ', '')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'oversized numeric telemetry'; Code = 'openal-pool-format'
        Stdout = $healthyPool.Replace(
            'backing=12410880', 'backing=18446744073709551616')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL uint32 overflow'; Code = 'openal-pool-format'
        Stdout = $healthyPool.Replace('live=0', 'live=4294967296')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'heap uint32 overflow'; Code = 'heapovf-format'
        Stdout = $healthyHeap.Replace('q=1', 'q=4294967296')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'heap marker case drift'; Code = 'heapovf-format'
        Stdout = $healthyHeap.Replace('heapovf:', 'HEAPOVF:')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'slab uint32 overflow'; Code = 'roomslab-format'
        Stdout = $rawSlab.Replace('a/f/r/ma/mc=4032/0/0/0/0',
            'a/f/r/ma/mc=4032/0/4294967296/0/0')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'external signed result overflow'; Code = 'roomslabx-format'
        Stdout = $externalFirstState.Replace(
            'state/fault/sys/result=1/0/1/10',
            'state/fault/sys/result=1/0/1/2147483648')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'external signed orphan underflow'; Code = 'roomslabx-format'
        Stdout = $externalFirstState.Replace(
            'orphan=-1/0/0', 'orphan=-2147483649/0/0')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'slab without external pair'; Code = 'roomslab-pairing'
        Stdout = $rawSlab; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'external without slab pair'; Code = 'roomslab-pairing'
        Stdout = $rawSlabExternal; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'ready audio without pool init'; Code = 'openal-pool-missing'
        Stdout = $healthyAudio; Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'guest heap allocation failure'; Code = 'heap-allocation-failed'
        Stdout = 'guest heap allocation failed: request=12 owner=0x003e5ade reason=ledger-reserve-failed'
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'bad_alloc'; Code = 'bad-alloc'; Stdout = ''
        Native = 'bad_alloc diagnostic: request=12 owner=0x003e5ade'; Game = ''
    },
    [pscustomobject]@{
        Name = 'room slab corruption'; Code = 'roomslab-corrupt'; Stdout = ''
        Native = 'guest stop: fault=room-entry slab is corrupted'; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL A005 attribution'; Code = 'openal-a005'
        Stdout = 'KAGE VITA OPENAL A005: n=1 site=005be811 seq=7'
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'game OpenAL 40965'; Code = 'openal-error-40965'; Stdout = ''
        Native = ''
        Game = '[ERROR] - SoundSourcePlatformBase::SetVolume() encountered OpenAL error: 40965'
    },
    [pscustomobject]@{
        Name = 'OpenAL audio init failure'; Code = 'openal-audio-init'
        Stdout = 'KAGE VITA AUDIO INIT: status=source-capacity-mismatch stage=openal'
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL manager capacity'; Code = 'openal-audio-capacity'
        Stdout = $healthyAudio.Replace('manager_sources=64', 'manager_sources=63')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL device capacity'; Code = 'openal-audio-capacity'
        Stdout = $healthyAudio.Replace('device_sources=80', 'device_sources=79')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL stream headroom'; Code = 'openal-audio-capacity'
        Stdout = $healthyAudio.Replace('stream_headroom=16', 'stream_headroom=15')
        Native = ''; Game = ''
    },
    [pscustomobject]@{
        Name = 'OpenAL snapshot unavailable'; Code = 'openal-pool-snapshot'
        Stdout = 'KAGE VITA OPENAL POOL: phase=init snapshot=unavailable'
        Native = ''; Game = ''
    }
)
foreach ($case in $simpleCases) {
    Assert-FailureCode -Name $case.Name -Code $case.Code `
        -Stdout $case.Stdout -Native $case.Native -Game $case.Game
}

$poolCases = @(
    @('manager failure', 'openal-manager-fail',
        $healthyPool.Replace('phase=init', 'phase=manager-fail')),
    @('A005 pool phase', 'openal-a005',
        $healthyPool.Replace('phase=init', 'phase=a005')),
    @('unknown pool phase', 'openal-pool-phase',
        $healthyPool.Replace('phase=init', 'phase=unknown')),
    @('pool state', 'openal-pool-state',
        $healthyPool.Replace('state=2', 'state=3')),
    @('pool init', 'openal-pool-init',
        $healthyPool.Replace('init=0', 'init=1')),
    @('pool backing', 'openal-pool-backing',
        $healthyPool.Replace('backing=12410880', 'backing=1')),
    @('pool failure count', 'openal-pool-failure',
        $healthyPool.Replace('pool_fail=0', 'pool_fail=1')),
    @('pool stranded count', 'openal-pool-stranded',
        $healthyPool.Replace('stranded=0', 'stranded=1')),
    @('pool corruption count', 'openal-pool-corrupt',
        $healthyPool.Replace('corrupt=0', 'corrupt=1')),
    @('pool pre-init allocation', 'openal-pool-preinit',
        $healthyPool.Replace('pre=0', 'pre=1')),
    @('pool live accounting', 'openal-pool-accounting',
        $healthyPool.Replace('ops=0/0/0', 'ops=1/0/0')),
    @('pool peak accounting', 'openal-pool-accounting',
        $healthyPool.Replace('live=0 bytes=0 peak=0/0',
            'live=1 bytes=8 peak=0/0').Replace(
                'ops=0/0/0', 'ops=1/0/0'))
)
foreach ($case in $poolCases) {
    Assert-FailureCode -Name $case[0] -Code $case[1] -Stdout $case[2]
}

$heapCases = @(
    @('pool-failure edge', 'heapovf-pool-failure',
        $healthyHeap.Replace('e=first-use', 'e=pool-failure')),
    @('terminal edge', 'heapovf-terminal',
        $healthyHeap.Replace('e=first-use', 'e=terminal')),
    @('unknown edge', 'heapovf-edge',
        $healthyHeap.Replace('e=first-use', 'e=unknown')),
    @('pool failure field', 'heapovf-pool-failure',
        $healthyHeap.Replace('nf/pf=0/0', 'nf/pf=0/1')),
    @('consumed terminal field', 'heapovf-consumed-terminal',
        $healthyHeap.Replace('cons=0', 'cons=1')),
    @('stranded field', 'heapovf-stranded',
        $healthyHeap.Replace('raw/str/int=1/0/', 'raw/str/int=1/1/')),
    @('mspace state', 'heapovf-state',
        $healthyHeap.Replace('state=1', 'state=2')),
    @('terminal field', 'heapovf-terminal',
        $healthyHeap.Replace('term=0', 'term=1')),
    @('accounting-valid field', 'heapovf-accounting',
        $healthyHeap.Replace('valid/sat=1/0', 'valid/sat=0/0')),
    @('saturation field', 'heapovf-saturated',
        $healthyHeap.Replace('valid/sat=1/0', 'valid/sat=1/1'))
)
foreach ($case in $heapCases) {
    Assert-FailureCode -Name $case[0] -Code $case[1] -Stdout $case[2]
}

$slabCases = @(
    @('terminal edge', 'roomslab-terminal',
        $rawSlab.Replace('e=page-power2', 'e=terminal')),
    @('unknown edge', 'roomslab-edge',
        $rawSlab.Replace('e=page-power2', 'e=unknown')),
    @('unexpected free', 'roomslab-unexpected-free',
        $rawSlab.Replace('unexpected=0', 'unexpected=1')),
    @('rejected operation', 'roomslab-rejected',
        $rawSlab.Replace('reject=0', 'reject=1')),
    @('terminal field', 'roomslab-terminal',
        $rawSlab.Replace('term/sat=0/0', 'term/sat=1/0')),
    @('saturated field', 'roomslab-saturated',
        $rawSlab.Replace('term/sat=0/0', 'term/sat=0/1')),
    @('page sum', 'roomslab-accounting',
        $rawSlab.Replace('pages=2(2+0)', 'pages=3(2+0)')),
    @('moving live', 'roomslab-accounting',
        $rawSlab.Replace('4032/0/4032', '4032/4033/4033')),
    @('peak live', 'roomslab-accounting',
        $rawSlab.Replace('4032/0/4032', '4032/0/4031')),
    @('allocation delta', 'roomslab-accounting',
        $rawSlab.Replace('a/f/r/ma/mc=4032/0/0/0/0',
            'a/f/r/ma/mc=4033/0/0/0/0')),
    @('move attempts', 'roomslab-accounting',
        $rawSlab.Replace('a/f/r/ma/mc=4032/0/0/0/0',
            'a/f/r/ma/mc=4032/0/0/1/0')),
    @('moving without attempt', 'roomslab-accounting',
        $rawSlab.Replace('live/move/peak=4032/0/4032',
            'live/move/peak=4032/1/4032')),
    @('moves', 'roomslab-accounting',
        $rawSlab.Replace('a/f/r/ma/mc=4032/0/0/0/0',
            'a/f/r/ma/mc=4032/0/0/0/1')),
    @('known free sum', 'roomslab-accounting',
        $rawSlab.Replace('known=0/0', 'known=1/0')),
    @('slot capacity', 'roomslab-accounting',
        $rawSlab.Replace('live/move/peak=4032/0/4032',
            'live/move/peak=8065/0/8065').Replace(
                'a/f/r/ma/mc=4032/0/0/0/0',
                'a/f/r/ma/mc=8065/0/0/0/0')),
    @('raw page count', 'roomslab-raw-accounting',
        $rawSlab.Replace('raw=2/131072', 'raw=1/131072')),
    @('raw requested bytes', 'roomslab-raw-accounting',
        $rawSlab.Replace('raw=2/131072', 'raw=2/65536')),
    @('retry interval', 'roomslab-accounting',
        $rawSlab.Replace('retry=0/0', 'retry=4033/0')),
    @('retry suppression', 'roomslab-accounting',
        $rawSlab.Replace('retry=0/0', 'retry=0/1')),
    @('raw page cap', 'roomslab-accounting',
        $rawSlab.Replace('pages=2(2+0)', 'pages=97(97+0)').Replace(
            'raw=2/131072', 'raw=97/6356992'))
)
foreach ($case in $slabCases) {
    Assert-FailureCode -Name ("slab " + $case[0]) -Code $case[1] `
        -Stdout $case[2]
}

$slabExternalCases = @(
    @('terminal edge', 'roomslabx-edge',
        $externalFirstState.Replace('e=external-first', 'e=terminal')),
    @('post-UID failure', 'roomslabx-post-uid',
        $externalFirstState.Replace('alloc/oom/post=1/0/0',
            'alloc/oom/post=1/0/1')),
    @('OOM accounting', 'roomslabx-accounting',
        $externalFirstState.Replace('alloc/oom/post=1/0/0',
            'alloc/oom/post=1/2/0')),
    @('attempt accounting', 'roomslabx-accounting',
        $externalFirstState.Replace('alloc/oom/post=1/0/0',
            'alloc/oom/post=2/0/0')),
    @('rollback attempt', 'roomslabx-rollback',
        $externalFirstState.Replace('rb=0/0', 'rb=1/0')),
    @('rollback failure', 'roomslabx-rollback',
        $externalFirstState.Replace('rb=0/0', 'rb=1/1')),
    @('reset attempt', 'roomslabx-reset',
        $externalFirstState.Replace('reset=0/0', 'reset=1/0')),
    @('reset failure', 'roomslabx-reset',
        $externalFirstState.Replace('reset=0/0', 'reset=1/1')),
    @('counter saturation', 'roomslabx-saturated',
        $externalFirstState.Replace('xsat=0', 'xsat=1')),
    @('orphan UID', 'roomslabx-orphan',
        $externalFirstState.Replace('orphan=-1/0/0', 'orphan=7/0/0')),
    @('orphan requested bytes', 'roomslabx-orphan',
        $externalFirstState.Replace('orphan=-1/0/0', 'orphan=-1/1/0')),
    @('orphan retained bytes', 'roomslabx-orphan',
        $externalFirstState.Replace('orphan=-1/0/0', 'orphan=-1/0/1')),
    @('external state', 'roomslabx-state',
        $externalFirstState.Replace('state/fault/sys/result=1/0/1/10',
            'state/fault/sys/result=2/0/1/10')),
    @('external fault', 'roomslabx-fault',
        $externalFirstState.Replace('state/fault/sys/result=1/0/1/10',
            'state/fault/sys/result=1/18/1/10')),
    @('external syscall enum', 'roomslabx-accounting',
        $externalFirstState.Replace('state/fault/sys/result=1/0/1/10',
            'state/fault/sys/result=1/0/5/10')),
    @('external none syscall result', 'roomslabx-accounting',
        $rawSlabExternal.Replace('state/fault/sys/result=1/0/0/0',
            'state/fault/sys/result=1/0/0/1')),
    @('chunk ceiling', 'roomslabx-backing',
        $externalFirstState.Replace('chunks=1', 'chunks=13').Replace(
            '1052672/1048576/1056768',
            '13684736/13631488/13737984')),
    @('requested backing', 'roomslabx-backing',
        $externalFirstState.Replace('1052672/1048576/1056768',
            '1/1048576/1056768')),
    @('usable backing', 'roomslabx-backing',
        $externalFirstState.Replace('1052672/1048576/1056768',
            '1052672/1/1056768')),
    @('retained backing', 'roomslabx-backing',
        $externalFirstState.Replace('1052672/1048576/1056768',
            '1052672/1048576/1'))
)
foreach ($case in $slabExternalCases) {
    Assert-FailureCode -Name ("external slab " + $case[0]) -Code $case[1] `
        -Stdout $case[2]
}

$duplicate = Invoke-HealthFixture `
    -Stdout 'bad_alloc diagnostic: request=12' `
    -Native 'bad_alloc diagnostic: request=12'
if ($duplicate.FailureCount -ne 1 -or $duplicate.BadAllocRecords -ne 2) {
    throw 'Mirrored stdout/native evidence did not deduplicate the failure code.'
}
$hostile = @($healthyPool.Replace('pool_fail=0', 'pool_fail=1'),
    $healthyStageNominal[0].Replace(
        'valid/term/sat=1/0/0', 'valid/term/sat=0/1/1'),
    'bad_alloc diagnostic: request=12',
    'guest heap allocation failed: request=12') -join "`n"
$disabled = Invoke-HealthFixture -Stdout $hostile -Native $hostile `
    -Game 'OpenAL error: 40965' -Required $false
if (-not $disabled.Passed -or $disabled.FailureCount -ne 0 -or
        $disabled.BadAllocRecords -ne 0 -or
        $disabled.OpenALPoolRecords -ne 0 -or
        $disabled.StageMemoryAllSourceRecords -ne 0 -or
        $disabled.StageMemoryAuthoritativeSource -ne 'none' -or
        $disabled.StageMemoryAuthoritativeRecords -ne 0 -or
        $disabled.StageMemoryLevelPairs -ne 0 -or
        $disabled.StageMemoryLoadSuccessPairs -ne 0 -or
        $disabled.StageMemoryLoadFailurePairs -ne 0 -or
        $disabled.StageMemoryReuseRecords -ne 0 -or
        $disabled.FloorLifeRequired -or
        $disabled.FloorLifePresent -or
        $disabled.FloorLifeAllSourceRecords -ne 0 -or
        $disabled.FloorLifeAuthoritativeSource -ne 'none' -or
        $disabled.FloorLifeAuthoritativeRecords -ne 0 -or
        $disabled.FloorLifeEpoch -ne 0 -or
        $disabled.FloorLifeBootstraps -ne 0 -or
        $disabled.FloorLifeRollovers -ne 0 -or
        $disabled.FloorLifePhase -ne 'none') {
    throw 'Disabled opt-in contract changed legacy runner behavior.'
}

$runnerText = [IO.File]::ReadAllText($runnerPath)
foreach ($requiredText in @(
        'CurrentStdout = $preStopOutput',
        'CurrentNativeLog = $currentNativeEvidence',
        'CurrentGameLog = $currentGameEvidence',
        'FloorLifeRequired = [bool]$RequireIsaacFloorLifetime',
        '[IO.File]::ReadAllText($nativeLogCurrent)',
        '[IO.File]::ReadAllText($gameLogCurrent)',
        'isaac_runtime_health_stagemem_all_source_records=',
        'isaac_runtime_health_stagemem_authoritative_source=',
        'isaac_runtime_health_stagemem_authoritative_records=',
        'isaac_runtime_health_stagemem_level_pairs=',
        'isaac_runtime_health_stagemem_load_success_pairs=',
        'isaac_runtime_health_stagemem_load_failure_pairs=',
        'isaac_runtime_health_stagemem_reuse_records=',
        'isaac_floor_lifetime_required=',
        'isaac_floor_lifetime_present=',
        'isaac_runtime_health_floorlife_all_source_records=',
        'isaac_runtime_health_floorlife_authoritative_source=',
        'isaac_runtime_health_floorlife_authoritative_records=',
        'isaac_runtime_health_floorlife_epoch=',
        'isaac_runtime_health_floorlife_bootstraps=',
        'isaac_runtime_health_floorlife_rollovers=',
        'isaac_runtime_health_floorlife_phase=',
        'isaac_runtime_health_failure_count=',
        'isaac_runtime_health_failure_codes=',
        'if ($isaacRuntimeHealthFailure) { exit 1 }')) {
    if (-not $runnerText.Contains($requiredText)) {
        throw "Runner runtime-health integration is missing: $requiredText"
    }
}
foreach ($forbiddenText in @(
        '[IO.File]::ReadAllText($nativeLogPreRun)',
        '[IO.File]::ReadAllText($gameLogPreRun)')) {
    if ($runnerText.Contains($forbiddenText)) {
        throw "Stale pre-run evidence entered runtime health: $forbiddenText"
    }
}

Write-Host ('Vita3K Isaac runtime-health contract: PASS ' +
    '(floor/stage exact pairing and lifetime algebra; source authority; ' +
    'sealed Stage3/Hybrid regressions; hostile fixtures; legacy OFF)')
