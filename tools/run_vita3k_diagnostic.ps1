<#
.SYNOPSIS
Runs one PID-bound Vita3K boot/menu/room soak and preserves its evidence.

.DESCRIPTION
The defaults expect Vita3K's keyboard mapping to expose Enter as Vita Start and
X as Vita Cross. The script sends one Start for the title followed by three
Cross confirmations for FILE SELECT / NEW RUN / character select. Override the
virtual keys or use -NoMenuInput for a boot-only run. Screenshots, when enabled,
capture only the exact emulator window. -ExerciseGameplay opt-in cycles the
default Vita3K W/A/S/D and I/J/K/L stick bindings during the soak; it never
guesses or edits the user's controller configuration. The existing game log
and the port's native first-fault log are moved into the evidence directory
before launch because Vita3K currently ignores SCE_O_TRUNC; otherwise stale
tail bytes can be misattributed to the run. -RequireIsaacRuntimeHealth opts into
the exact heap/slab/stage-memory/OpenAL/bad_alloc health gate over current-run
evidence only. -RequireIsaacFloorLifetime additionally requires the optional
floor-lifetime stream; it implies the runtime-health gate.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Vita3KExe,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [string]$TitleId = 'ISAACR001',
    [string]$Vita3KDataDir = (Join-Path $env:APPDATA 'Vita3K\Vita3K'),
    [string]$ExpectedEbootSha256 = '',
    [string]$ExpectedOpenALConfigSha256 = '',
    [string]$StallDebuggerPath = '',
    [int]$BootTimeoutSeconds = 150,
    [int]$SoakSeconds = 600,
    [int]$PresentStallSeconds = 45,
    [int]$StartPressCount = 1,
    [int]$ConfirmCount = 3,
    [int]$ConfirmHoldMilliseconds = 700,
    [int]$ConfirmGapMilliseconds = 4000,
    [int]$MenuInputDelaySeconds = 5,
    [byte]$StartVirtualKey = 0x0D,
    [byte]$StartScanCode = 0x1C,
    [byte]$ConfirmVirtualKey = 0x58,
    [byte]$ConfirmScanCode = 0x2D,
    [switch]$NoMenuInput,
    [switch]$ExerciseGameplay,
    [int]$ExerciseStartDelaySeconds = 5,
    [int]$ExerciseHoldMilliseconds = 700,
    [int]$ExerciseGapMilliseconds = 100,
    [switch]$AllowFocusClickFallback,
    [switch]$RequireInputTransitions,
    [switch]$CaptureWindow,
    [switch]$SkipSaveBackup,
    [switch]$RequireIsaacRuntimeHealth,
    [switch]$RequireIsaacFloorLifetime
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Confirm-InstalledOpenALConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedSha256
    )

    if ($ExpectedSha256 -notmatch '\A[0-9a-fA-F]{64}\z') {
        throw 'Expected OpenAL config SHA-256 must contain exactly 64 hex digits.'
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Installed OpenAL config not found: $Path"
    }
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedSha256.ToUpperInvariant()) {
        throw "Installed OpenAL config hash mismatch: $actualHash"
    }
    return $actualHash
}

function Get-OpenALAudioContractFailure {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Output,
        [Parameter(Mandatory = $true)]
        [bool]$Required
    )

    if (-not $Required) { return $null }
    if ($Output -match '(?im)Missing file[^\r\n]*alsoft\.conf') {
        return 'installed OpenAL config was missing at runtime'
    }
    if ($Output -match
            'KAGE VITA AUDIO INIT:\s+status=source-capacity-mismatch(?:\s|$)') {
        return 'OpenAL source-capacity mismatch was logged'
    }
    if ($Output -notmatch
            'KAGE VITA AUDIO INIT:\s+status=ready(?:\s|$)') {
        return 'OpenAL ready marker was not logged'
    }
    return $null
}

function Get-IsaacRuntimeHealthContract {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$CurrentStdout,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$CurrentNativeLog,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$CurrentGameLog,
        [Parameter(Mandatory = $true)]
        [bool]$Required,
        [Parameter(Mandatory = $true)]
        [bool]$FloorLifeRequired
    )

    $failureCodes = New-Object System.Collections.Generic.List[string]
    $failureSet = New-Object System.Collections.Generic.HashSet[string] `
        ([StringComparer]::Ordinal)
    $addFailure = {
        param([string]$Code)
        if ($failureSet.Add($Code)) {
            [void]$failureCodes.Add($Code)
        }
    }
    $records = [ordered]@{
        HeapAllocationFailure = 0
        HeapOverflow = 0
        RoomSlab = 0
        RoomSlabExternal = 0
        StageMemory = 0
        FloorLife = 0
        OpenALPool = 0
        OpenALA005 = 0
        OpenALError40965 = 0
        BadAlloc = 0
    }
    $audioReadyRecords = 0
    $openALPoolInitRecords = 0
    [uint64]$uint32Max = [uint32]::MaxValue
    [int64]$int32Min = [int32]::MinValue
    [int64]$int32Max = [int32]::MaxValue
    $stageMemoryAuthoritativeSource = 'none'
    $stageMemoryAuthoritativeRecords = 0
    $stageMemoryLevelPairs = 0
    $stageMemoryLoadSuccessPairs = 0
    $stageMemoryLoadFailurePairs = 0
    $stageMemoryReuseRecords = 0
    $stageMemorySequenceValid = $true
    $stageMemoryOrdered = @()
    $floorLifeAuthoritativeSource = 'none'
    $floorLifeAuthoritativeRecords = 0
    $floorLifePresent = $false
    [uint64]$floorLifeEpoch = 0
    [uint64]$floorLifeBootstraps = 0
    [uint64]$floorLifeRollovers = 0
    $floorLifePhase = 'none'
    $effectiveRequired = $Required -or $FloorLifeRequired

    if ($effectiveRequired) {
        # Stage-memory records are written to the native file log before
        # the same complete line is printed to stdout.  The native file is
        # captured after stop while stdout is deliberately frozen before it,
        # so a lower-priority source may be only a prefix.  Select on the
        # presence of any marker, including malformed case drift: malformed
        # native evidence must not be hidden by falling back to healthy stdout.
        if ($CurrentNativeLog -match '\bstagemem:') {
            $stageMemoryAuthoritativeSource = 'native'
        } elseif ($CurrentStdout -match '\bstagemem:') {
            $stageMemoryAuthoritativeSource = 'stdout'
        } elseif ($CurrentGameLog -match '\bstagemem:') {
            $stageMemoryAuthoritativeSource = 'game'
        }

        # Every numeric field is emitted with C's lowercase %x.  Limiting each
        # capture to eight hex digits makes the complete shape also its uint32
        # range check.  Prefixes added by the native/Vita3K loggers are allowed;
        # nothing may follow the producer payload on the same line.
        $stageMemoryPattern =
            '\bstagemem:\s+q=([0-9a-f]{1,8})\s+' +
            'lq/rq=([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'e=([a-z-]+)\s+ctx_valid=([0-9a-f]{1,8})\s+' +
            'active=([0-9a-f]{1,8})\s+' +
            'ls=([0-9a-f]{1,8})\s+lt=([0-9a-f]{1,8})\s+' +
            'rs=([0-9a-f]{1,8})\s+mode=([0-9a-f]{1,8})\s+' +
            'ok=([0-9a-f]{1,8})\s+' +
            'mi=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'ledger=([0-9a-f]{1,8})\s+' +
            'ov=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'slab=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'valid/term/sat=([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s*$'
        $stageMemoryEvents = @(
            'level-begin', 'room-reuse', 'room-load-begin',
            'room-after-unload', 'room-load-end', 'level-end')
        $stageMemoryParsed =
            New-Object System.Collections.Generic.List[object]
        $stageMemoryAuthoritativeShapeValid = $true
        $stageMemoryAuthoritativeContextValid = $true
        $stageSources = @(
            [pscustomobject]@{ Name = 'stdout'; Text = $CurrentStdout },
            [pscustomobject]@{ Name = 'native'; Text = $CurrentNativeLog },
            [pscustomobject]@{ Name = 'game'; Text = $CurrentGameLog }
        )
        foreach ($stageSource in $stageSources) {
            foreach ($stageLine in
                    [regex]::Split($stageSource.Text, '\r\n|\n|\r')) {
                # -match is intentionally case-insensitive so case drift still
                # enters the exact case-sensitive parser and fails closed.
                if ($stageLine -notmatch '\bstagemem:') { continue }
                ++$records.StageMemory
                $isAuthoritative = $stageSource.Name -eq
                    $stageMemoryAuthoritativeSource
                if ($isAuthoritative) {
                    ++$stageMemoryAuthoritativeRecords
                }
                $stageMarkerCount = [regex]::Matches(
                    $stageLine, '\bstagemem:',
                    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
                ).Count
                if ($stageMarkerCount -ne 1) {
                    & $addFailure 'stagemem-format'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeShapeValid = $false
                    }
                    continue
                }
                $stageShape = [regex]::Match(
                    $stageLine, $stageMemoryPattern)
                if (-not $stageShape.Success) {
                    & $addFailure 'stagemem-format'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeShapeValid = $false
                    }
                    continue
                }

                $stageRecord = [pscustomobject]@{
                    Q = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[1].Value, 16)
                    LevelQ = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[2].Value, 16)
                    RoomQ = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[3].Value, 16)
                    Event = $stageShape.Groups[4].Value
                    ContextValid = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[5].Value, 16)
                    Active = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[6].Value, 16)
                    LevelStage = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[7].Value, 16)
                    LevelType = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[8].Value, 16)
                    RoomStage = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[9].Value, 16)
                    RoomMode = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[10].Value, 16)
                    Result = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[11].Value, 16)
                    Ledger = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[16].Value, 16)
                    SlabLive = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[24].Value, 16)
                    Valid = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[27].Value, 16)
                    Terminal = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[28].Value, 16)
                    Saturated = [uint64][Convert]::ToUInt32(
                        $stageShape.Groups[29].Value, 16)
                }
                if ($stageRecord.Event -notin $stageMemoryEvents) {
                    & $addFailure 'stagemem-lifecycle'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeContextValid = $false
                    }
                }
                if ($stageRecord.ContextValid -ne 1) {
                    & $addFailure 'stagemem-context'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeContextValid = $false
                    }
                }
                if ($stageRecord.Valid -ne 1) {
                    & $addFailure 'stagemem-invalid'
                }
                if ($stageRecord.Terminal -ne 0) {
                    & $addFailure 'stagemem-terminal'
                }
                if ($stageRecord.Saturated -ne 0) {
                    & $addFailure 'stagemem-saturated'
                }
                if ($stageRecord.Active -gt 1) {
                    & $addFailure 'stagemem-lifecycle'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeContextValid = $false
                    }
                }
                $stageResultValid = if (
                        $stageRecord.Event -eq 'room-load-end') {
                    $stageRecord.Result -le 1
                } else {
                    $stageRecord.Result -eq $uint32Max
                }
                if (-not $stageResultValid) {
                    & $addFailure 'stagemem-lifecycle'
                    if ($isAuthoritative) {
                        $stageMemoryAuthoritativeContextValid = $false
                    }
                }
                if ($isAuthoritative) {
                    [void]$stageMemoryParsed.Add($stageRecord)
                }
            }
        }

        # q is assigned while the heap lock is held, but formatting and both
        # writes happen after unlock.  Concurrent callers may therefore reach
        # the file in another physical order.  The complete 1..N set is the
        # loss/duplicate check; sorted q is the sole lifecycle order.
        $stageMemorySequenceValid =
            $stageMemoryAuthoritativeShapeValid -and
            $stageMemoryParsed.Count -eq $stageMemoryAuthoritativeRecords
        $stageMemoryOrdered = @()
        if ($stageMemorySequenceValid -and
                $stageMemoryAuthoritativeRecords -ne 0) {
            $stageMemoryOrdered = @($stageMemoryParsed | Sort-Object Q)
            for ($stageIndex = 0;
                 $stageIndex -lt $stageMemoryOrdered.Count; ++$stageIndex) {
                [uint64]$expectedStageQ = $stageIndex + 1
                if ($stageMemoryOrdered[$stageIndex].Q -ne $expectedStageQ) {
                    & $addFailure 'stagemem-sequence'
                    $stageMemorySequenceValid = $false
                    break
                }
            }
        }

        if ($stageMemorySequenceValid -and
                $stageMemoryAuthoritativeContextValid -and
                $stageMemoryOrdered.Count -ne 0) {
            $stageLevelOpen = $false
            [uint64]$stageLevelQ = 0
            [uint64]$stageLevelStage = $uint32Max
            [uint64]$stageLevelType = $uint32Max
            $stageRoomOpen = $false
            $stageRoomAfterUnload = $false
            [uint64]$stageRoomQ = 0
            [uint64]$stageRoomStage = $uint32Max
            [uint64]$stageRoomMode = $uint32Max
            $stageLifecycleValid = $true

            foreach ($stageRecord in $stageMemoryOrdered) {
                $clearStageRoom = $false
                $clearStageLevel = $false
                $countStageReuse = $false
                $countStageLoadSuccess = $false
                $countStageLoadFailure = $false
                $countStageLevel = $false
                switch ($stageRecord.Event) {
                    'level-begin' {
                        if ($stageLevelOpen -or $stageRoomOpen) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $stageLevelOpen = $true
                        $stageLevelQ = $stageRecord.Q
                        $stageLevelStage = $stageRecord.LevelStage
                        $stageLevelType = $stageRecord.LevelType
                        $stageRoomOpen = $false
                        $stageRoomAfterUnload = $false
                        $stageRoomQ = 0
                        $stageRoomStage = $uint32Max
                        $stageRoomMode = $uint32Max
                    }
                    'room-reuse' {
                        if ($stageRoomOpen) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $stageRoomAfterUnload = $false
                        $stageRoomQ = $stageRecord.Q
                        $stageRoomStage = $stageRecord.RoomStage
                        $stageRoomMode = $stageRecord.RoomMode
                        $countStageReuse = $true
                    }
                    'room-load-begin' {
                        if ($stageRoomOpen) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $stageRoomOpen = $true
                        $stageRoomAfterUnload = $false
                        $stageRoomQ = $stageRecord.Q
                        $stageRoomStage = $stageRecord.RoomStage
                        $stageRoomMode = $stageRecord.RoomMode
                    }
                    'room-after-unload' {
                        if (-not $stageRoomOpen -or
                                $stageRoomAfterUnload) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $stageRoomAfterUnload = $true
                    }
                    'room-load-end' {
                        if (-not $stageRoomOpen -or
                                ($stageRecord.Result -eq 1 -and
                                 -not $stageRoomAfterUnload)) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $clearStageRoom = $true
                        if ($stageRecord.Result -eq 1) {
                            $countStageLoadSuccess = $true
                        } else {
                            $countStageLoadFailure = $true
                        }
                    }
                    'level-end' {
                        if (-not $stageLevelOpen -or $stageRoomOpen) {
                            $stageLifecycleValid = $false
                            break
                        }
                        $clearStageLevel = $true
                        $countStageLevel = $true
                    }
                    default {
                        $stageLifecycleValid = $false
                    }
                }
                if (-not $stageLifecycleValid) {
                    & $addFailure 'stagemem-lifecycle'
                    break
                }

                [uint64]$expectedStageActive = if ($stageLevelOpen) {
                    1
                } else { 0 }
                [uint64]$expectedStageLevelQ = if ($stageLevelOpen) {
                    $stageLevelQ
                } else { 0 }
                [uint64]$expectedStageLevelStage = if ($stageLevelOpen) {
                    $stageLevelStage
                } else { $uint32Max }
                [uint64]$expectedStageLevelType = if ($stageLevelOpen) {
                    $stageLevelType
                } else { $uint32Max }
                [uint64]$expectedStageRoomStage = if ($stageRoomQ -ne 0) {
                    $stageRoomStage
                } else { $uint32Max }
                [uint64]$expectedStageRoomMode = if ($stageRoomQ -ne 0) {
                    $stageRoomMode
                } else { $uint32Max }
                if ($stageRecord.Active -ne $expectedStageActive -or
                        $stageRecord.LevelQ -ne $expectedStageLevelQ -or
                        $stageRecord.LevelStage -ne
                            $expectedStageLevelStage -or
                        $stageRecord.LevelType -ne $expectedStageLevelType -or
                        $stageRecord.RoomQ -ne $stageRoomQ -or
                        $stageRecord.RoomStage -ne
                            $expectedStageRoomStage -or
                        $stageRecord.RoomMode -ne $expectedStageRoomMode) {
                    & $addFailure 'stagemem-lifecycle'
                    $stageLifecycleValid = $false
                    break
                }

                if ($countStageReuse) { ++$stageMemoryReuseRecords }
                if ($countStageLoadSuccess) {
                    ++$stageMemoryLoadSuccessPairs
                }
                if ($countStageLoadFailure) {
                    ++$stageMemoryLoadFailurePairs
                }
                if ($countStageLevel) { ++$stageMemoryLevelPairs }
                if ($clearStageRoom) {
                    $stageRoomOpen = $false
                    $stageRoomAfterUnload = $false
                }
                if ($clearStageLevel) {
                    $stageLevelOpen = $false
                    $stageLevelQ = 0
                    $stageLevelStage = $uint32Max
                    $stageLevelType = $uint32Max
                    $stageRoomOpen = $false
                    $stageRoomAfterUnload = $false
                    $stageRoomQ = 0
                    $stageRoomStage = $uint32Max
                    $stageRoomMode = $uint32Max
                }
            }
            if ($stageLifecycleValid -and
                    ($stageLevelOpen -or $stageRoomOpen)) {
                & $addFailure 'stagemem-pairing'
            }
        }

        # floorlife is a diagnostic extension of the stage-memory stream.  It
        # is optional for legacy candidates.  Once any source contains its
        # marker, however, the highest-priority source is authoritative and
        # must contain exactly one complete record for every authoritative
        # stagemem q/event.  Native is captured after stop, while stdout is a
        # pre-stop snapshot and may be only a prefix; malformed native data
        # therefore never falls back to a healthy lower-priority source.
        $floorLifePresent =
            $CurrentNativeLog -match '\bfloorlife:' -or
            $CurrentStdout -match '\bfloorlife:' -or
            $CurrentGameLog -match '\bfloorlife:'
        if ($FloorLifeRequired -and -not $floorLifePresent) {
            & $addFailure 'floorlife-missing'
        }
        if ($CurrentNativeLog -match '\bfloorlife:') {
            $floorLifeAuthoritativeSource = 'native'
        } elseif ($CurrentStdout -match '\bfloorlife:') {
            $floorLifeAuthoritativeSource = 'stdout'
        } elseif ($CurrentGameLog -match '\bfloorlife:') {
            $floorLifeAuthoritativeSource = 'game'
        }

        # All numeric values are C lowercase %x uint32 values.  The first
        # heap quartet is total/unscoped/prior/current live entries and the
        # second is requested bytes.  hphase is the current-floor live split by
        # birth phase (level-init/room-load/play), entries then bytes.  slab
        # and sphase use the same age/phase order for live slots.
        $floorLifePattern =
            '\bfloorlife:\s+q=([0-9a-f]{1,8})\s+' +
            'e=([a-z-]+)\s+epoch=([0-9a-f]{1,8})\s+' +
            'boot/roll=([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'phase=([a-z-]+)\s+' +
            'heap=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8}),' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'hphase=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8}),([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'slab=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s+' +
            'sphase=([0-9a-f]{1,8})/([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})\s+' +
            'valid/term/sat=([0-9a-f]{1,8})/' +
            '([0-9a-f]{1,8})/([0-9a-f]{1,8})\s*$'
        $floorLifeEvents = @(
            'level-begin', 'room-reuse', 'room-load-begin',
            'room-after-unload', 'room-load-end', 'level-end')
        $floorLifePhases = @('level-init', 'room-load', 'play')
        $floorLifeParsed =
            New-Object System.Collections.Generic.List[object]
        $floorLifeAuthoritativeShapeValid = $true
        $floorLifeAuthoritativeSemanticValid = $true
        $floorSources = @(
            [pscustomobject]@{ Name = 'stdout'; Text = $CurrentStdout },
            [pscustomobject]@{ Name = 'native'; Text = $CurrentNativeLog },
            [pscustomobject]@{ Name = 'game'; Text = $CurrentGameLog }
        )
        foreach ($floorSource in $floorSources) {
            foreach ($floorLine in
                    [regex]::Split($floorSource.Text, '\r\n|\n|\r')) {
                # PowerShell -match is intentionally case-insensitive here:
                # case drift enters the exact case-sensitive parser and fails.
                if ($floorLine -notmatch '\bfloorlife:') { continue }
                ++$records.FloorLife
                $isAuthoritative = $floorSource.Name -eq
                    $floorLifeAuthoritativeSource
                if ($isAuthoritative) {
                    ++$floorLifeAuthoritativeRecords
                }
                $floorMarkerCount = [regex]::Matches(
                    $floorLine, '\bfloorlife:',
                    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
                ).Count
                if ($floorMarkerCount -ne 1) {
                    & $addFailure 'floorlife-format'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeShapeValid = $false
                    }
                    continue
                }
                $floorShape = [regex]::Match(
                    $floorLine, $floorLifePattern)
                if (-not $floorShape.Success) {
                    & $addFailure 'floorlife-format'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeShapeValid = $false
                    }
                    continue
                }

                $floorValues = New-Object 'uint64[]' 28
                foreach ($floorGroup in @(1) + @(3..5) + @(7..30)) {
                    $floorValueIndex = if ($floorGroup -eq 1) {
                        0
                    } elseif ($floorGroup -le 5) {
                        $floorGroup - 2
                    } else {
                        $floorGroup - 3
                    }
                    $floorValues[$floorValueIndex] =
                        [uint64][Convert]::ToUInt32(
                            $floorShape.Groups[$floorGroup].Value, 16)
                }
                $floorRecord = [pscustomobject]@{
                    Q = $floorValues[0]
                    Event = $floorShape.Groups[2].Value
                    Epoch = $floorValues[1]
                    Bootstraps = $floorValues[2]
                    Rollovers = $floorValues[3]
                    Phase = $floorShape.Groups[6].Value
                    HeapTotal = $floorValues[4]
                    HeapUnscoped = $floorValues[5]
                    HeapPrior = $floorValues[6]
                    HeapCurrent = $floorValues[7]
                    HeapBytesTotal = $floorValues[8]
                    HeapBytesUnscoped = $floorValues[9]
                    HeapBytesPrior = $floorValues[10]
                    HeapBytesCurrent = $floorValues[11]
                    HeapPhaseLevel = $floorValues[12]
                    HeapPhaseRoom = $floorValues[13]
                    HeapPhasePlay = $floorValues[14]
                    HeapBytesPhaseLevel = $floorValues[15]
                    HeapBytesPhaseRoom = $floorValues[16]
                    HeapBytesPhasePlay = $floorValues[17]
                    SlabTotal = $floorValues[18]
                    SlabUnscoped = $floorValues[19]
                    SlabPrior = $floorValues[20]
                    SlabCurrent = $floorValues[21]
                    SlabPhaseLevel = $floorValues[22]
                    SlabPhaseRoom = $floorValues[23]
                    SlabPhasePlay = $floorValues[24]
                    Valid = $floorValues[25]
                    Terminal = $floorValues[26]
                    Saturated = $floorValues[27]
                }
                if ($floorRecord.Event -notin $floorLifeEvents) {
                    & $addFailure 'floorlife-event'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($floorRecord.Phase -notin $floorLifePhases) {
                    & $addFailure 'floorlife-phase'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                $floorEventPhaseMismatch =
                    ($floorRecord.Bootstraps -ne 1 -or
                     $floorRecord.Epoch -eq 0 -or
                     $floorRecord.Epoch -gt $floorRecord.Q) -or
                    ($floorRecord.Q -eq 1 -and
                     $floorRecord.Event -ne 'level-begin' -and
                     $floorRecord.Event -ne 'room-load-begin') -or
                    ($floorRecord.Q -eq 1 -and
                     ($floorRecord.HeapCurrent -ne 0 -or
                      $floorRecord.HeapBytesCurrent -ne 0 -or
                      $floorRecord.SlabCurrent -ne 0)) -or
                    ($floorRecord.Rollovers -eq 0 -and
                     ($floorRecord.HeapPrior -ne 0 -or
                      $floorRecord.HeapBytesPrior -ne 0 -or
                      $floorRecord.SlabPrior -ne 0)) -or
                    ($floorRecord.Event -eq 'level-begin' -and
                     $floorRecord.Phase -ne 'level-init') -or
                    (($floorRecord.Event -eq 'room-load-begin' -or
                      $floorRecord.Event -eq 'room-after-unload') -and
                     $floorRecord.Phase -ne 'room-load') -or
                    ($floorRecord.Event -eq 'level-end' -and
                     $floorRecord.Phase -ne 'play') -or
                    ($floorRecord.Event -eq 'room-reuse' -and
                     $floorRecord.Phase -eq 'room-load') -or
                    ($floorRecord.Event -eq 'level-begin' -and
                     ($floorRecord.HeapCurrent -ne 0 -or
                      $floorRecord.HeapBytesCurrent -ne 0 -or
                      $floorRecord.SlabCurrent -ne 0))
                if ($floorEventPhaseMismatch) {
                    & $addFailure 'floorlife-lifecycle'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($floorRecord.Valid -ne 1) {
                    & $addFailure 'floorlife-invalid'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($floorRecord.Terminal -ne 0) {
                    & $addFailure 'floorlife-terminal'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($floorRecord.Saturated -ne 0) {
                    & $addFailure 'floorlife-saturated'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($floorRecord.Epoch -ne
                            $floorRecord.Bootstraps +
                            $floorRecord.Rollovers -or
                        $floorRecord.HeapTotal -ne
                            $floorRecord.HeapUnscoped +
                            $floorRecord.HeapPrior +
                            $floorRecord.HeapCurrent -or
                        $floorRecord.HeapBytesTotal -ne
                            $floorRecord.HeapBytesUnscoped +
                            $floorRecord.HeapBytesPrior +
                            $floorRecord.HeapBytesCurrent -or
                        $floorRecord.HeapPhaseLevel +
                            $floorRecord.HeapPhaseRoom +
                            $floorRecord.HeapPhasePlay -ne
                            $floorRecord.HeapCurrent -or
                        $floorRecord.HeapBytesPhaseLevel +
                            $floorRecord.HeapBytesPhaseRoom +
                            $floorRecord.HeapBytesPhasePlay -ne
                            $floorRecord.HeapBytesCurrent -or
                        $floorRecord.SlabTotal -ne
                            $floorRecord.SlabUnscoped +
                            $floorRecord.SlabPrior +
                            $floorRecord.SlabCurrent -or
                        $floorRecord.SlabPhaseLevel +
                            $floorRecord.SlabPhaseRoom +
                            $floorRecord.SlabPhasePlay -ne
                            $floorRecord.SlabCurrent -or
                        ($floorRecord.HeapTotal -eq 0 -and
                         $floorRecord.HeapBytesTotal -ne 0) -or
                        ($floorRecord.HeapUnscoped -eq 0 -and
                         $floorRecord.HeapBytesUnscoped -ne 0) -or
                        ($floorRecord.HeapPrior -eq 0 -and
                         $floorRecord.HeapBytesPrior -ne 0) -or
                        ($floorRecord.HeapCurrent -eq 0 -and
                         $floorRecord.HeapBytesCurrent -ne 0) -or
                        ($floorRecord.HeapPhaseLevel -eq 0 -and
                         $floorRecord.HeapBytesPhaseLevel -ne 0) -or
                        ($floorRecord.HeapPhaseRoom -eq 0 -and
                         $floorRecord.HeapBytesPhaseRoom -ne 0) -or
                        ($floorRecord.HeapPhasePlay -eq 0 -and
                         $floorRecord.HeapBytesPhasePlay -ne 0)) {
                    & $addFailure 'floorlife-accounting'
                    if ($isAuthoritative) {
                        $floorLifeAuthoritativeSemanticValid = $false
                    }
                }
                if ($isAuthoritative) {
                    [void]$floorLifeParsed.Add($floorRecord)
                }
            }
        }

        $floorLifeSequenceValid =
            $floorLifeAuthoritativeShapeValid -and
            $floorLifeParsed.Count -eq $floorLifeAuthoritativeRecords
        $floorLifeOrdered = @()
        if ($floorLifeSequenceValid -and
                $floorLifeAuthoritativeRecords -ne 0) {
            $floorLifeOrdered = @($floorLifeParsed | Sort-Object Q)
            for ($floorIndex = 0;
                 $floorIndex -lt $floorLifeOrdered.Count; ++$floorIndex) {
                [uint64]$expectedFloorQ = $floorIndex + 1
                if ($floorLifeOrdered[$floorIndex].Q -ne $expectedFloorQ) {
                    & $addFailure 'floorlife-sequence'
                    $floorLifeSequenceValid = $false
                    break
                }
            }
        }

        $floorLifePairingValid = -not $floorLifePresent
        if ($floorLifePresent -and $floorLifeSequenceValid -and
                $stageMemorySequenceValid -and
                $floorLifeAuthoritativeSource -eq
                    $stageMemoryAuthoritativeSource -and
                $floorLifeOrdered.Count -eq $stageMemoryOrdered.Count -and
                $floorLifeOrdered.Count -ne 0) {
            $floorLifePairingValid = $true
            for ($floorIndex = 0;
                 $floorIndex -lt $floorLifeOrdered.Count; ++$floorIndex) {
                if ($floorLifeOrdered[$floorIndex].Q -ne
                        $stageMemoryOrdered[$floorIndex].Q -or
                        $floorLifeOrdered[$floorIndex].Event -ne
                        $stageMemoryOrdered[$floorIndex].Event) {
                    $floorLifePairingValid = $false
                    break
                }
            }
        }
        if ($floorLifePresent -and -not $floorLifePairingValid) {
            & $addFailure 'floorlife-pairing'
        }

        if ($floorLifePairingValid -and $floorLifePresent) {
            for ($floorIndex = 0;
                 $floorIndex -lt $floorLifeOrdered.Count; ++$floorIndex) {
                $floorRecord = $floorLifeOrdered[$floorIndex]
                $stageRecord = $stageMemoryOrdered[$floorIndex]
                if ($floorRecord.HeapTotal -ne $stageRecord.Ledger -or
                        $floorRecord.SlabTotal -ne $stageRecord.SlabLive -or
                        ($floorRecord.Valid -eq 1 -and
                         $stageRecord.Valid -ne 1)) {
                    & $addFailure 'floorlife-cross-domain'
                    $floorLifeAuthoritativeSemanticValid = $false
                    break
                }
            }
        }

        if ($floorLifePairingValid -and $floorLifePresent -and
                $floorLifeAuthoritativeSemanticValid) {
            [uint64]$expectedFloorEpoch = 0
            [uint64]$expectedFloorBootstraps = 0
            [uint64]$expectedFloorRollovers = 0
            $expectedFloorPhase = 'pre-floor'
            $floorLifeLifecycleValid = $true
            $floorSeenLevelInit = $false
            $floorSeenRoomLoad = $false
            $floorSeenPlay = $false
            [uint64]$previousFloorEpoch = 0
            [uint64]$previousCombinedUnscoped = 0
            [uint64]$previousCombinedPrior = 0
            for ($floorIndex = 0;
                 $floorIndex -lt $floorLifeOrdered.Count; ++$floorIndex) {
                $floorRecord = $floorLifeOrdered[$floorIndex]
                $stageRecord = $stageMemoryOrdered[$floorIndex]
                if ($floorIndex -eq 0) {
                    $expectedFloorBootstraps = 1
                    $expectedFloorEpoch = 1
                    if ($floorRecord.Event -eq 'level-begin') {
                        $expectedFloorPhase = 'level-init'
                    } elseif ($floorRecord.Event -eq 'room-load-begin') {
                        $expectedFloorPhase = 'room-load'
                    } else {
                        $floorLifeLifecycleValid = $false
                    }
                } elseif ($floorRecord.Event -eq 'level-begin') {
                    if ($expectedFloorRollovers -eq $uint32Max) {
                        $floorLifeLifecycleValid = $false
                    } else {
                        ++$expectedFloorRollovers
                        ++$expectedFloorEpoch
                        $expectedFloorPhase = 'level-init'
                        $floorSeenLevelInit = $false
                        $floorSeenRoomLoad = $false
                        $floorSeenPlay = $false
                    }
                } else {
                    switch ($floorRecord.Event) {
                        'room-load-begin' {
                            $expectedFloorPhase = 'room-load'
                        }
                        'room-after-unload' {
                            $expectedFloorPhase = 'room-load'
                        }
                        'room-load-end' {
                            $expectedFloorPhase = if (
                                    $stageRecord.Active -eq 1) {
                                'level-init'
                            } else { 'play' }
                        }
                        'level-end' {
                            $expectedFloorPhase = 'play'
                        }
                        'room-reuse' {
                            # A reuse probe observes but does not change phase.
                        }
                    }
                }
                $unseenFloorPhaseHasBirths =
                    (-not $floorSeenLevelInit -and
                     ($floorRecord.HeapPhaseLevel -ne 0 -or
                      $floorRecord.HeapBytesPhaseLevel -ne 0 -or
                      $floorRecord.SlabPhaseLevel -ne 0)) -or
                    (-not $floorSeenRoomLoad -and
                     ($floorRecord.HeapPhaseRoom -ne 0 -or
                      $floorRecord.HeapBytesPhaseRoom -ne 0 -or
                      $floorRecord.SlabPhaseRoom -ne 0)) -or
                    (-not $floorSeenPlay -and
                     ($floorRecord.HeapPhasePlay -ne 0 -or
                      $floorRecord.HeapBytesPhasePlay -ne 0 -or
                      $floorRecord.SlabPhasePlay -ne 0))
                [uint64]$combinedFloorUnscoped =
                    $floorRecord.HeapUnscoped + $floorRecord.SlabUnscoped
                [uint64]$combinedFloorPrior =
                    $floorRecord.HeapPrior + $floorRecord.SlabPrior
                $floorAgeBucketGrew = $false
                if ($floorIndex -ne 0) {
                    if ($combinedFloorUnscoped -gt
                            $previousCombinedUnscoped -or
                            ($expectedFloorEpoch -eq $previousFloorEpoch -and
                             $combinedFloorPrior -gt
                                $previousCombinedPrior)) {
                        $floorAgeBucketGrew = $true
                    }
                }
                if (-not $floorLifeLifecycleValid -or
                        $floorRecord.Epoch -ne $expectedFloorEpoch -or
                        $floorRecord.Bootstraps -ne
                            $expectedFloorBootstraps -or
                        $floorRecord.Rollovers -ne
                            $expectedFloorRollovers -or
                        $floorRecord.Phase -ne $expectedFloorPhase -or
                        $unseenFloorPhaseHasBirths -or
                        $floorAgeBucketGrew -or
                        ($floorIndex -eq 0 -and
                         ($floorRecord.HeapCurrent -ne 0 -or
                          $floorRecord.HeapBytesCurrent -ne 0 -or
                          $floorRecord.SlabCurrent -ne 0)) -or
                        ($floorRecord.Event -eq 'level-begin' -and
                         ($floorRecord.HeapCurrent -ne 0 -or
                          $floorRecord.HeapBytesCurrent -ne 0 -or
                          $floorRecord.SlabCurrent -ne 0))) {
                    & $addFailure 'floorlife-lifecycle'
                    $floorLifeLifecycleValid = $false
                    break
                }
                switch ($expectedFloorPhase) {
                    'level-init' { $floorSeenLevelInit = $true }
                    'room-load' { $floorSeenRoomLoad = $true }
                    'play' { $floorSeenPlay = $true }
                }
                $previousFloorEpoch = $expectedFloorEpoch
                $previousCombinedUnscoped = $combinedFloorUnscoped
                $previousCombinedPrior = $combinedFloorPrior
            }
            if ($floorLifeLifecycleValid -and
                    $expectedFloorPhase -ne 'play') {
                & $addFailure 'floorlife-pairing'
            }
            if ($floorLifeLifecycleValid) {
                $floorLifeEpoch = $expectedFloorEpoch
                $floorLifeBootstraps = $expectedFloorBootstraps
                $floorLifeRollovers = $expectedFloorRollovers
                $floorLifePhase = $expectedFloorPhase
            }
        }

        # The hybrid logger emits one roomslabx snapshot for every roomslab
        # snapshot with the same edge.  The native current-run log is closed
        # after the guest stops, while the stdout snapshot is deliberately
        # taken before stop and can end between those two logger calls.  Use
        # native as the authoritative pair sequence when it has telemetry;
        # otherwise fall back to stdout (then game evidence).  Every present
        # line in every source is still format-checked below.
        $pairingSource = if ($CurrentNativeLog -match '\broomslabx?:') {
            $CurrentNativeLog
        } elseif ($CurrentStdout -match '\broomslabx?:') {
            $CurrentStdout
        } else {
            $CurrentGameLog
        }
        foreach ($source in @($pairingSource)) {
            $slabEdges = New-Object System.Collections.Generic.List[string]
            $externalEdges = New-Object System.Collections.Generic.List[string]
            foreach ($sourceLine in [regex]::Split($source, '\r?\n')) {
                $slabEdgeMatch = [regex]::Match(
                    $sourceLine, '\broomslab:\s+e=([^\s]+)')
                if ($slabEdgeMatch.Success) {
                    [void]$slabEdges.Add($slabEdgeMatch.Groups[1].Value)
                }
                $externalEdgeMatch = [regex]::Match(
                    $sourceLine, '\broomslabx:\s+e=([^\s]+)')
                if ($externalEdgeMatch.Success) {
                    [void]$externalEdges.Add(
                        $externalEdgeMatch.Groups[1].Value)
                }
            }
            if ($slabEdges.Count -ne $externalEdges.Count) {
                & $addFailure 'roomslab-pairing'
            } else {
                for ($edgeIndex = 0; $edgeIndex -lt $slabEdges.Count;
                     ++$edgeIndex) {
                    if ($slabEdges[$edgeIndex] -ne
                            $externalEdges[$edgeIndex]) {
                        & $addFailure 'roomslab-pairing'
                        break
                    }
                }
            }
        }
        $evidence = @(
            $CurrentStdout,
            $CurrentNativeLog,
            $CurrentGameLog
        ) -join "`n"
        foreach ($line in [regex]::Split($evidence, '\r?\n')) {
            if ($line -match '\bguest heap allocation failed:') {
                ++$records.HeapAllocationFailure
                & $addFailure 'heap-allocation-failed'
            }
            if ($line -match '(?i)\bbad_alloc\b') {
                ++$records.BadAlloc
                & $addFailure 'bad-alloc'
            }
            if ($line -match 'room-entry slab is corrupted') {
                & $addFailure 'roomslab-corrupt'
            }
            if ($line -match 'KAGE VITA OPENAL A005:') {
                ++$records.OpenALA005
                & $addFailure 'openal-a005'
            }
            if ($line -match '(?i)OpenAL error:\s*40965\b') {
                ++$records.OpenALError40965
                & $addFailure 'openal-error-40965'
            }
            if ($line -match 'KAGE VITA AUDIO INIT:') {
                if ($line -notmatch '\bstatus=ready(?:\s|$)') {
                    & $addFailure 'openal-audio-init'
                } else {
                    ++$audioReadyRecords
                    $audioShape = [regex]::Match(
                        $line,
                        '\bKAGE VITA AUDIO INIT:\s+status=ready\s+' +
                        'stage=complete\s+device=0x[0-9a-fA-F]{8}\s+' +
                        'context=0x[0-9a-fA-F]{8}\s+' +
                        'al=0x[0-9a-fA-F]{8}\s+' +
                        'manager_sources=([0-9]{1,10})\s+' +
                        'device_sources=([0-9]{1,10})\s+' +
                        'stream_headroom=([0-9]{1,10})\s+' +
                        'buffers=([0-9]{1,10})\s+' +
                        'pump=main-loop\+room-cooperative\s+' +
                        'guest_thread=off\s*$')
                    if (-not $audioShape.Success) {
                        & $addFailure 'openal-audio-format'
                    } elseif (@(1..4 | Where-Object {
                            [uint64]$audioShape.Groups[$_].Value -gt $uint32Max
                        }).Count -ne 0) {
                        & $addFailure 'openal-audio-format'
                    } elseif ([uint64]$audioShape.Groups[1].Value -ne 64 -or
                            [uint64]$audioShape.Groups[2].Value -ne 80 -or
                            [uint64]$audioShape.Groups[3].Value -ne 16 -or
                            [uint64]$audioShape.Groups[4].Value -ne 64) {
                        & $addFailure 'openal-audio-capacity'
                    }
                }
            }

            if ($line -match 'KAGE VITA OPENAL POOL:') {
                ++$records.OpenALPool
                if ($line -match
                        '\bKAGE VITA OPENAL POOL:\s+phase=[^\s]+\s+' +
                        'snapshot=unavailable\s*$') {
                    & $addFailure 'openal-pool-snapshot'
                    continue
                }
                $poolShape = [regex]::Match(
                    $line,
                    '\bKAGE VITA OPENAL POOL:\s+phase=([^\s]+)\s+' +
                    'state=([0-9]{1,10})\s+init=([0-9]{1,10})\s+' +
                    'backing=([0-9]{1,10})\s+live=([0-9]{1,10})\s+' +
                    'bytes=([0-9]{1,10})\s+peak=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+ops=([0-9]{1,10})/' +
                    '([0-9]{1,10})/([0-9]{1,10})\s+' +
                    'pool_fail=([0-9]{1,10})\s+' +
                    'stranded=([0-9]{1,10})\s+' +
                    'corrupt=([0-9]{1,10})\s+pre=([0-9]{1,10})\s*$')
                if (-not $poolShape.Success) {
                    & $addFailure 'openal-pool-format'
                    continue
                }
                if (@(2..15 | Where-Object {
                        [uint64]$poolShape.Groups[$_].Value -gt $uint32Max
                    }).Count -ne 0) {
                    & $addFailure 'openal-pool-format'
                    continue
                }
                $phase = $poolShape.Groups[1].Value
                [uint64]$poolState = $poolShape.Groups[2].Value
                [uint64]$poolInit = $poolShape.Groups[3].Value
                [uint64]$poolBacking = $poolShape.Groups[4].Value
                [uint64]$poolLive = $poolShape.Groups[5].Value
                [uint64]$poolBytes = $poolShape.Groups[6].Value
                [uint64]$poolPeakLive = $poolShape.Groups[7].Value
                [uint64]$poolPeakBytes = $poolShape.Groups[8].Value
                [uint64]$poolAllocations = $poolShape.Groups[9].Value
                [uint64]$poolFrees = $poolShape.Groups[10].Value
                [uint64]$poolFailures = $poolShape.Groups[12].Value
                [uint64]$poolStranded = $poolShape.Groups[13].Value
                [uint64]$poolCorrupt = $poolShape.Groups[14].Value
                [uint64]$poolPreinit = $poolShape.Groups[15].Value
                if ($phase -eq 'init') {
                    ++$openALPoolInitRecords
                } elseif ($phase -eq 'a005') {
                    & $addFailure 'openal-a005'
                } elseif ($phase -eq 'manager-fail') {
                    & $addFailure 'openal-manager-fail'
                } elseif ($phase -ne 'final') {
                    & $addFailure 'openal-pool-phase'
                }
                if ($poolState -ne 2) {
                    & $addFailure 'openal-pool-state'
                }
                if ($poolInit -ne 0) {
                    & $addFailure 'openal-pool-init'
                }
                if ($poolBacking -ne 12410880) {
                    & $addFailure 'openal-pool-backing'
                }
                if ($poolFailures -ne 0) {
                    & $addFailure 'openal-pool-failure'
                }
                if ($poolStranded -ne 0) {
                    & $addFailure 'openal-pool-stranded'
                }
                if ($poolCorrupt -ne 0) {
                    & $addFailure 'openal-pool-corrupt'
                }
                if ($poolPreinit -ne 0) {
                    & $addFailure 'openal-pool-preinit'
                }
                if ($poolAllocations -lt $poolFrees -or
                        $poolAllocations - $poolFrees -ne $poolLive -or
                        $poolPeakLive -lt $poolLive -or
                        $poolPeakBytes -lt $poolBytes) {
                    & $addFailure 'openal-pool-accounting'
                }
            }

            if ($line -match '\bheapovf:') {
                ++$records.HeapOverflow
                $heapShape = [regex]::Match(
                    $line,
                        '\bheapovf:\s+e=[^\s]+\s+' +
                        'q=[0-9]{1,10}\s+op=[0-9]{1,10}\s+' +
                        'req=[0-9]{1,10}\s+live=[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'peak=[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'a/f/r=[0-9]{1,10}/[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'n2p/p2n=[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'nf/pf=[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'cons=[0-9]{1,10}\s+' +
                        'raw/str/int=[0-9]{1,10}/[0-9]{1,10}/' +
                        '[0-9]{1,10}/[0-9]{1,10}\s+' +
                        'state=[0-9]{1,10}\s+term=[0-9]{1,10}\s+' +
                        'valid/sat=[0-9]{1,10}/[0-9]{1,10}\s*$')
                if (-not $heapShape.Success) {
                    & $addFailure 'heapovf-format'
                    continue
                }
                $heapPayload = $line.Substring($heapShape.Index)
                if (@([regex]::Matches(
                            $heapPayload,
                            '(?<=[=/])[0-9]{1,10}(?=[/\s]|$)') |
                        Where-Object { [uint64]$_.Value -gt $uint32Max }
                    ).Count -ne 0) {
                    & $addFailure 'heapovf-format'
                    continue
                }
                if ($line -match '\be=([^\s]+)') {
                    $edge = $Matches[1]
                    if ($edge -eq 'pool-failure') {
                        & $addFailure 'heapovf-pool-failure'
                    } elseif ($edge -eq 'terminal') {
                        & $addFailure 'heapovf-terminal'
                    } elseif ($edge -notin @(
                            'first-use', 'first-free', 'first-realloc',
                            'native-to-pool', 'pool-to-native', 'final')) {
                        & $addFailure 'heapovf-edge'
                    }
                }
                if ($line -match '\bnf/pf=[0-9]+/([1-9][0-9]*)') {
                    & $addFailure 'heapovf-pool-failure'
                }
                if ($line -match '\bcons=([1-9][0-9]*)') {
                    & $addFailure 'heapovf-consumed-terminal'
                }
                if ($line -match '\braw/str/int=[0-9]+/([1-9][0-9]*)/') {
                    & $addFailure 'heapovf-stranded'
                }
                if ($line -match '\bstate=([0-9]+)' -and
                        [uint64]$Matches[1] -ne 1) {
                    & $addFailure 'heapovf-state'
                }
                if ($line -match '\bterm=([1-9][0-9]*)') {
                    & $addFailure 'heapovf-terminal'
                }
                if ($line -match '\bvalid/sat=([0-9]+)/([0-9]+)') {
                    if ([uint64]$Matches[1] -ne 1) {
                        & $addFailure 'heapovf-accounting'
                    }
                    if ([uint64]$Matches[2] -ne 0) {
                        & $addFailure 'heapovf-saturated'
                    }
                }
            }

            if ($line -match '\broomslab:') {
                ++$records.RoomSlab
                $shape = [regex]::Match(
                    $line,
                    '\broomslab:\s+e=([^\s]+)\s+' +
                    'pages=([0-9]{1,10})\(([0-9]{1,10})\+' +
                    '([0-9]{1,10})\)\s+' +
                    'live/move/peak=([0-9]{1,10})/([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+' +
                    'a/f/r/ma/mc=([0-9]{1,10})/([0-9]{1,10})/' +
                    '([0-9]{1,10})/([0-9]{1,10})/([0-9]{1,10})\s+' +
                    'fb=([0-9]{1,10})\s+known=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+unexpected=([0-9]{1,10})\s+' +
                    'reject=([0-9]{1,10})\s+raw=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+retry=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+term/sat=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s*$')
                if (-not $shape.Success) {
                    & $addFailure 'roomslab-format'
                    continue
                }
                if (@(2..23 | Where-Object {
                        [uint64]$shape.Groups[$_].Value -gt $uint32Max
                    }).Count -ne 0) {
                    & $addFailure 'roomslab-format'
                    continue
                }
                $edge = $shape.Groups[1].Value
                if ($edge -eq 'terminal') {
                    & $addFailure 'roomslab-terminal'
                } elseif ($edge -notin @(
                        'first-use', 'page-power2', 'fallback', 'final',
                        'external-first', 'external-chunk-power2',
                        'external-oom-power2')) {
                    & $addFailure 'roomslab-edge'
                }
                [uint64]$pages = $shape.Groups[2].Value
                [uint64]$rawPages = $shape.Groups[3].Value
                [uint64]$externalPages = $shape.Groups[4].Value
                [uint64]$live = $shape.Groups[5].Value
                [uint64]$moving = $shape.Groups[6].Value
                [uint64]$peak = $shape.Groups[7].Value
                [uint64]$allocations = $shape.Groups[8].Value
                [uint64]$frees = $shape.Groups[9].Value
                [uint64]$reallocations = $shape.Groups[10].Value
                [uint64]$moveAttempts = $shape.Groups[11].Value
                [uint64]$moves = $shape.Groups[12].Value
                [uint64]$fallbacks = $shape.Groups[13].Value
                [uint64]$knownNormal = $shape.Groups[14].Value
                [uint64]$knownStage = $shape.Groups[15].Value
                [uint64]$unexpected = $shape.Groups[16].Value
                [uint64]$rejected = $shape.Groups[17].Value
                [uint64]$rawInternal = $shape.Groups[18].Value
                [uint64]$rawRequested = $shape.Groups[19].Value
                [uint64]$retryRemaining = $shape.Groups[20].Value
                [uint64]$retrySuppressed = $shape.Groups[21].Value
                [uint64]$terminal = $shape.Groups[22].Value
                [uint64]$saturated = $shape.Groups[23].Value
                if ($unexpected -ne 0) {
                    & $addFailure 'roomslab-unexpected-free'
                }
                if ($rejected -ne 0) {
                    & $addFailure 'roomslab-rejected'
                }
                if ($terminal -ne 0) {
                    & $addFailure 'roomslab-terminal'
                }
                if ($saturated -ne 0) {
                    & $addFailure 'roomslab-saturated'
                }
                if ($pages -ne $rawPages + $externalPages -or
                        $pages -gt 192 -or $rawPages -gt 96 -or
                        $externalPages -gt 192 -or $moving -gt $live -or
                        $peak -lt $live -or $live -gt $pages * 4032 -or
                        $allocations -lt $frees -or
                        $allocations - $frees -ne $live -or
                        $moves -gt $moveAttempts -or
                        $moving -gt $moveAttempts - $moves -or
                        $moveAttempts -gt $reallocations -or
                        $moves -gt $frees -or
                        $knownNormal + $knownStage + $unexpected -gt $frees -or
                        $retryRemaining -gt 4032 -or
                        ($pages -eq 192 -and $retryRemaining -ne 0) -or
                        $retrySuppressed -gt $fallbacks) {
                    & $addFailure 'roomslab-accounting'
                }
                if ($rawInternal -ne $rawPages -or
                        $rawRequested -ne $rawPages * 65536) {
                    & $addFailure 'roomslab-raw-accounting'
                }
            }

            if ($line -match '\broomslabx:') {
                ++$records.RoomSlabExternal
                $externalShape = [regex]::Match(
                    $line,
                    '\broomslabx:\s+e=([^\s]+)\s+' +
                    'chunks=([0-9]{1,10})\s+' +
                    'alloc/oom/post=([0-9]{1,10})/([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+rb=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+reset=([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+xsat=([0-9]{1,10})\s+' +
                    'bytes=req/use/ret=([0-9]{1,10})/([0-9]{1,10})/' +
                    '([0-9]{1,10})\s+orphan=(-?[0-9]{1,10})/' +
                    '([0-9]{1,10})/([0-9]{1,10})\s+' +
                    'state/fault/sys/result=([0-9]{1,10})/' +
                    '([0-9]{1,10})/([0-9]{1,10})/' +
                    '(-?[0-9]{1,10})\s*$')
                if (-not $externalShape.Success) {
                    & $addFailure 'roomslabx-format'
                    continue
                }
                $externalUnsignedGroups = @(2..13) + @(15..19)
                [int64]$externalOrphanUid = $externalShape.Groups[14].Value
                [int64]$externalLastResult = $externalShape.Groups[20].Value
                if (@($externalUnsignedGroups | Where-Object {
                        [uint64]$externalShape.Groups[$_].Value -gt $uint32Max
                    }).Count -ne 0 -or
                        $externalOrphanUid -lt $int32Min -or
                        $externalOrphanUid -gt $int32Max -or
                        $externalLastResult -lt $int32Min -or
                        $externalLastResult -gt $int32Max) {
                    & $addFailure 'roomslabx-format'
                    continue
                }
                $edge = $externalShape.Groups[1].Value
                [uint64]$chunks = $externalShape.Groups[2].Value
                [uint64]$attempts = $externalShape.Groups[3].Value
                [uint64]$outOfMemory = $externalShape.Groups[4].Value
                [uint64]$postUid = $externalShape.Groups[5].Value
                [uint64]$rollbackAttempts = $externalShape.Groups[6].Value
                [uint64]$rollbackFailures = $externalShape.Groups[7].Value
                [uint64]$resetAttempts = $externalShape.Groups[8].Value
                [uint64]$resetFailures = $externalShape.Groups[9].Value
                [uint64]$externalSaturated = $externalShape.Groups[10].Value
                [uint64]$requested = $externalShape.Groups[11].Value
                [uint64]$usable = $externalShape.Groups[12].Value
                [uint64]$retained = $externalShape.Groups[13].Value
                [int64]$orphanUid = $externalShape.Groups[14].Value
                [uint64]$orphanRequested = $externalShape.Groups[15].Value
                [uint64]$orphanRetained = $externalShape.Groups[16].Value
                [uint64]$externalState = $externalShape.Groups[17].Value
                [uint64]$externalFault = $externalShape.Groups[18].Value
                [uint64]$lastSyscall = $externalShape.Groups[19].Value
                [int64]$lastResult = $externalShape.Groups[20].Value
                if ($edge -notin @(
                        'first-use', 'page-power2', 'fallback', 'final',
                        'external-first', 'external-chunk-power2',
                        'external-oom-power2')) {
                    & $addFailure 'roomslabx-edge'
                }
                if ($postUid -ne 0) {
                    & $addFailure 'roomslabx-post-uid'
                }
                if ($outOfMemory -gt $attempts -or
                        $postUid -gt $attempts -or
                        $outOfMemory + $postUid -gt $attempts -or
                        $attempts -ne $chunks + $outOfMemory + $postUid -or
                        $lastSyscall -gt 4 -or
                        ($lastSyscall -eq 0 -and $lastResult -ne 0)) {
                    & $addFailure 'roomslabx-accounting'
                }
                if ($rollbackAttempts -ne 0 -or $rollbackFailures -ne 0) {
                    & $addFailure 'roomslabx-rollback'
                }
                if ($resetAttempts -ne 0 -or $resetFailures -ne 0) {
                    & $addFailure 'roomslabx-reset'
                }
                if ($externalSaturated -ne 0) {
                    & $addFailure 'roomslabx-saturated'
                }
                if ($orphanUid -ne -1 -or $orphanRequested -ne 0 -or
                        $orphanRetained -ne 0) {
                    & $addFailure 'roomslabx-orphan'
                }
                if ($externalState -ne 1) {
                    & $addFailure 'roomslabx-state'
                }
                if ($externalFault -ne 0) {
                    & $addFailure 'roomslabx-fault'
                }
                if ($chunks -gt 12 -or
                        $requested -ne $chunks * 1052672 -or
                        $usable -ne $chunks * 1048576 -or
                        $retained -ne $chunks * 1056768) {
                    & $addFailure 'roomslabx-backing'
                }
            }
        }
        if ($audioReadyRecords -gt 0 -and $openALPoolInitRecords -eq 0) {
            & $addFailure 'openal-pool-missing'
        }
    }

    $failureMessage = ''
    if ($failureCodes.Count -ne 0) {
        $failureMessage = 'Isaac runtime health violation(s): ' +
            ($failureCodes -join ',')
    }
    return [pscustomobject]@{
        Required = $effectiveRequired
        FloorLifeRequired = $FloorLifeRequired
        FloorLifePresent = $floorLifePresent
        Passed = (-not $effectiveRequired -or $failureCodes.Count -eq 0)
        Failure = $failureMessage
        FailureCount = $failureCodes.Count
        FailureCodes = [string[]]$failureCodes.ToArray()
        HeapAllocationFailureRecords = $records.HeapAllocationFailure
        HeapOverflowRecords = $records.HeapOverflow
        RoomSlabRecords = $records.RoomSlab
        RoomSlabExternalRecords = $records.RoomSlabExternal
        StageMemoryAllSourceRecords = $records.StageMemory
        StageMemoryAuthoritativeSource = $stageMemoryAuthoritativeSource
        StageMemoryAuthoritativeRecords = $stageMemoryAuthoritativeRecords
        StageMemoryLevelPairs = $stageMemoryLevelPairs
        StageMemoryLoadSuccessPairs = $stageMemoryLoadSuccessPairs
        StageMemoryLoadFailurePairs = $stageMemoryLoadFailurePairs
        StageMemoryReuseRecords = $stageMemoryReuseRecords
        FloorLifeAllSourceRecords = $records.FloorLife
        FloorLifeAuthoritativeSource = $floorLifeAuthoritativeSource
        FloorLifeAuthoritativeRecords = $floorLifeAuthoritativeRecords
        FloorLifeEpoch = $floorLifeEpoch
        FloorLifeBootstraps = $floorLifeBootstraps
        FloorLifeRollovers = $floorLifeRollovers
        FloorLifePhase = $floorLifePhase
        OpenALPoolRecords = $records.OpenALPool
        OpenALA005Records = $records.OpenALA005
        OpenALError40965Records = $records.OpenALError40965
        BadAllocRecords = $records.BadAlloc
        StdoutChars = $CurrentStdout.Length
        NativeLogChars = $CurrentNativeLog.Length
        GameLogChars = $CurrentGameLog.Length
    }
}

function Get-DiagnosticAccessCounts {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$PreStopOutput,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$FinalOutput
    )

    $preHost = ([regex]::Matches(
        $PreStopOutput, 'EXCEPTION_ACCESS_VIOLATION')).Count
    $finalHost = ([regex]::Matches(
        $FinalOutput, 'EXCEPTION_ACCESS_VIOLATION')).Count
    $preArm = ([regex]::Matches(
        $PreStopOutput, 'Invalid read of uint')).Count
    $finalArm = ([regex]::Matches(
        $FinalOutput, 'Invalid read of uint')).Count
    return [pscustomobject]@{
        PreStopHost = $preHost
        ShutdownHost = [Math]::Max(0, $finalHost - $preHost)
        PreStopArm = $preArm
        ShutdownArm = [Math]::Max(0, $finalArm - $preArm)
    }
}

function Initialize-EvidenceDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
            throw "Evidence output is not a directory: $Path"
        }
        if (@(Get-ChildItem -LiteralPath $Path -Force).Count -ne 0) {
            throw "Refusing to overwrite non-empty evidence directory: $Path"
        }
    } else {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

if ($BootTimeoutSeconds -le 0 -or $SoakSeconds -lt 0 -or
        $PresentStallSeconds -le 0) {
    throw 'Soak must be non-negative; boot and present-stall timeouts must be positive.'
}
if ($StartPressCount -lt 0 -or $ConfirmCount -lt 0 -or
        $ConfirmHoldMilliseconds -lt 50 -or
        $ConfirmGapMilliseconds -lt 0 -or $MenuInputDelaySeconds -lt 0) {
    throw 'Invalid confirmation timing.'
}
if ($ExerciseStartDelaySeconds -lt 0 -or
        $ExerciseHoldMilliseconds -lt 50 -or
        $ExerciseGapMilliseconds -lt 20) {
    throw 'Invalid gameplay-exercise timing.'
}

$resolvedExe = (Resolve-Path -LiteralPath $Vita3KExe).Path
$workingDir = Split-Path -Parent $resolvedExe
$resolvedOutput = Initialize-EvidenceDirectory -Path $OutputDir
$resolvedStallDebugger = ''
if ($StallDebuggerPath) {
    $resolvedStallDebugger = (Resolve-Path -LiteralPath $StallDebuggerPath).Path
    if (-not (Test-Path -LiteralPath $resolvedStallDebugger -PathType Leaf)) {
        throw "Stall debugger is not a file: $resolvedStallDebugger"
    }
}

$installedEboot = Join-Path $Vita3KDataDir "ux0\app\$TitleId\eboot.bin"
if (-not (Test-Path -LiteralPath $installedEboot -PathType Leaf)) {
    throw "Installed eboot not found: $installedEboot"
}
$installedHash = (Get-FileHash -LiteralPath $installedEboot -Algorithm SHA256).Hash
if ($ExpectedEbootSha256 -and
        $installedHash -ne $ExpectedEbootSha256.ToUpperInvariant()) {
    throw "Installed eboot hash mismatch: $installedHash"
}

$openALAudioContractRequired = [bool]$ExpectedOpenALConfigSha256
$installedOpenALConfigHash = ''
if ($openALAudioContractRequired) {
    $installedOpenALConfig = Join-Path $Vita3KDataDir "ux0\app\$TitleId\alsoft.conf"
    $openALConfigCheck = @{
        Path = $installedOpenALConfig
        ExpectedSha256 = $ExpectedOpenALConfigSha256
    }
    $installedOpenALConfigHash = Confirm-InstalledOpenALConfig @openALConfigCheck
}

$existing = @(Get-Process -Name Vita3K -ErrorAction SilentlyContinue)
if ($existing.Count -ne 0) {
    throw 'Refusing to start while another Vita3K process exists.'
}

$stdoutPath = Join-Path $resolvedOutput 'vita3k.stdout.log'
$stderrPath = Join-Path $resolvedOutput 'vita3k.stderr.log'
$vita3kLiveLog = Join-Path $workingDir 'vita3k.log'
$vita3kSnapshot = Join-Path $resolvedOutput 'vita3k.pre-stop.log'
$beforeImage = Join-Path $resolvedOutput 'window-before-input.png'
$afterImage = Join-Path $resolvedOutput 'window-after-soak.png'
$stallImage = Join-Path $resolvedOutput 'window-stall.png'
$stallDumpPath = Join-Path $resolvedOutput 'stall-full.dmp'
$stallCdbLog = Join-Path $resolvedOutput 'stall-cdb.log'
$stallCdbStdout = Join-Path $resolvedOutput 'stall-cdb.stdout.log'
$stallCdbCommands = Join-Path $resolvedOutput 'stall-cdb-commands.txt'
$summaryPath = Join-Path $resolvedOutput 'diagnostic-summary.txt'
$saveSource = Join-Path $Vita3KDataDir "ux0\data\isaacr001\Documents"
$saveBackupRoot = Join-Path $resolvedOutput 'save-backup'
$saveBackupResult = 'none'
$gameLogPath = Join-Path $saveSource 'My Games\Binding of Isaac Repentance\log.txt'
$gameLogPreRun = Join-Path $resolvedOutput 'game-log.pre-run.txt'
$gameLogCurrent = Join-Path $resolvedOutput 'game-log.current.txt'
$gameLogRotation = 'source-missing'
$nativeLogPath = Join-Path $Vita3KDataDir `
    'ux0\data\isaacr001\first-arm-fault.log'
$nativeLogPreRun = Join-Path $resolvedOutput 'first-arm-fault.pre-run.log'
$nativeLogCurrent = Join-Path $resolvedOutput 'first-arm-fault.current.log'
$nativeLogRotation = 'source-missing'

if (-not $SkipSaveBackup -and
        (Test-Path -LiteralPath $saveSource -PathType Container)) {
    if (Test-Path -LiteralPath $saveBackupRoot) {
        throw "Refusing to overwrite existing save backup: $saveBackupRoot"
    }
    New-Item -ItemType Directory -Path $saveBackupRoot | Out-Null
    Copy-Item -LiteralPath $saveSource -Destination $saveBackupRoot -Recurse
    $saveBackupResult = (Join-Path $saveBackupRoot 'Documents')
}

Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
if ((Test-Path -LiteralPath $stdoutPath) -or
        (Test-Path -LiteralPath $stderrPath)) {
    throw 'Failed to clear pre-run Vita3K redirected output.'
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RepentogxmVita3KWin32 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct GUITHREADINFO {
        public uint cbSize, flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner;
        public IntPtr hwndMoveSize, hwndCaret;
        public RECT rcCaret;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx, dy;
        public uint mouseData, dwFlags, time;
        public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort virtualKey, scanCode;
        public uint flags, time;
        public UIntPtr extraInfo;
    }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mouse;
        [FieldOffset(0)] public KEYBDINPUT keyboard;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public INPUTUNION value; }
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", SetLastError=true)] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] private static extern bool GetGUIThreadInfo(uint id, ref GUITHREADINFO info);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", SetLastError=true)]
    private static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", SetLastError=true)]
    private static extern uint SendInput(uint count, INPUT[] input, int size);
    [DllImport("user32.dll")]
    private static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static bool SendKeyToExactForeground(
            IntPtr expected, uint expectedPid, byte vk, byte scan, bool down) {
        uint owner;
        if (GetForegroundWindow() != expected ||
                GetWindowThreadProcessId(expected, out owner) == 0 ||
                owner != expectedPid) return false;
        keybd_event(vk, scan, down ? 0u : 0x0002u, UIntPtr.Zero);
        return true;
    }
    public static void ReleaseGlobalKey(byte vk, byte scan) {
        keybd_event(vk, scan, 0x0002u, UIntPtr.Zero);
    }
    public static bool HasExactThreadFocus(IntPtr expected, uint expectedPid) {
        uint owner;
        uint thread = GetWindowThreadProcessId(expected, out owner);
        if (thread == 0 || owner != expectedPid) return false;
        GUITHREADINFO info = new GUITHREADINFO();
        info.cbSize = (uint)Marshal.SizeOf(typeof(GUITHREADINFO));
        if (!GetGUIThreadInfo(thread, ref info)) return false;
        return GetAncestor(info.hwndActive, 2u) == expected &&
            GetAncestor(info.hwndFocus, 2u) == expected;
    }
    public static bool ClickWindowForForeground(IntPtr h, uint expectedPid) {
        RECT rect;
        POINT clientOrigin = new POINT();
        POINT original;
        uint owner;
        if (!GetClientRect(h, out rect) || rect.Right <= rect.Left ||
                rect.Bottom <= rect.Top || !ClientToScreen(h, ref clientOrigin) ||
                !GetCursorPos(out original) || GetForegroundWindow() != h ||
                GetWindowThreadProcessId(h, out owner) == 0 ||
                owner != expectedPid) return false;
        const uint SWP_NOSIZE = 0x0001u;
        const uint SWP_NOMOVE = 0x0002u;
        const uint SWP_NOACTIVATE = 0x0010u;
        const uint SWP_SHOWWINDOW = 0x0040u;
        bool promoted = SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0,
            SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
        if (!promoted) return false;
        bool moved = false;
        uint sent = 0;
        try {
            // Click a quiet pixel inside the render client, not the Qt title bar.
            // Foreground ownership alone does not establish SDL keyboard focus.
            int clickX = clientOrigin.X + Math.Min(8, rect.Right - 1);
            int clickY = clientOrigin.Y + Math.Min(8, rect.Bottom - 1);
            moved = SetCursorPos(clickX, clickY);
            if (moved) {
                POINT hitPoint = new POINT();
                hitPoint.X = clickX;
                hitPoint.Y = clickY;
                IntPtr hit = WindowFromPoint(hitPoint);
                uint currentOwner;
                if (GetAncestor(hit, 2u) == h && GetForegroundWindow() == h &&
                        GetWindowThreadProcessId(h, out currentOwner) != 0 &&
                        currentOwner == expectedPid) {
                    INPUT[] input = new INPUT[2];
                    input[0].type = 0;
                    input[0].value.mouse.dwFlags = 0x0002u;
                    input[1].type = 0;
                    input[1].value.mouse.dwFlags = 0x0004u;
                    sent = SendInput(2, input, Marshal.SizeOf(typeof(INPUT)));
                    if (sent == 1) {
                        INPUT[] release = new INPUT[1];
                        release[0].type = 0;
                        release[0].value.mouse.dwFlags = 0x0004u;
                        SendInput(1, release, Marshal.SizeOf(typeof(INPUT)));
                    }
                }
            }
        } finally {
            SetCursorPos(original.X, original.Y);
            SetWindowPos(h, new IntPtr(-2), 0, 0, 0, 0,
                SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }
        return promoted && moved && sent == 2;
    }
}
"@

if ($CaptureWindow) {
    Add-Type -AssemblyName System.Drawing
}

$script:vita3kProcess = $null
$script:vita3kPid = 0
$script:heldVirtualKey = 0
$script:heldScanCode = 0
$script:capturedFailure = $null
$script:lastObservedPresentCount = [uint64]0
$script:vita3kExitCode = $null
$script:preStopOutput = ''
$script:currentRunOutputStart = 0
$script:stallDumpStatus = if ($resolvedStallDebugger) { 'armed' } else { 'disabled' }
$script:stallDumpFailure = ''
$script:stallWindowFailure = ''
$script:exerciseHeldKeys = @()
$script:exercisePulseCount = 0
$script:focusClickFallbackCount = 0
$script:inputClientPrimeCount = 0

function Get-ExactWindow {
    $script:vita3kProcess.Refresh()
    if ($script:vita3kProcess.HasExited) {
        $exitCode = Read-Vita3KExitCode
        throw "Vita3K exited early with code $exitCode."
    }
    $handle = $script:vita3kProcess.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) {
        throw 'Vita3K has no main window yet.'
    }
    [uint32]$owner = 0
    [void][RepentogxmVita3KWin32]::GetWindowThreadProcessId($handle, [ref]$owner)
    if ($owner -ne [uint32]$script:vita3kPid) {
        throw "Window owner $owner does not match Vita3K PID $script:vita3kPid."
    }
    return $handle
}

function Focus-ExactWindow {
    $handle = Get-ExactWindow
    $foreground = [RepentogxmVita3KWin32]::GetForegroundWindow()
    [uint32]$targetOwner = 0
    [uint32]$foregroundOwner = 0
    $targetThread = [RepentogxmVita3KWin32]::GetWindowThreadProcessId(
        $handle, [ref]$targetOwner)
    $foregroundThread = if ($foreground -ne [IntPtr]::Zero) {
        [RepentogxmVita3KWin32]::GetWindowThreadProcessId(
            $foreground, [ref]$foregroundOwner)
    } else {
        [uint32]0
    }
    $currentThread = [RepentogxmVita3KWin32]::GetCurrentThreadId()
    $attachedThreads = New-Object System.Collections.Generic.List[uint32]
    try {
        # A hidden automation process normally fails SetForegroundWindow's
        # foreground-lock policy.  Temporarily share its input queue with the
        # current foreground and exact target threads, then verify the exact
        # HWND again before any input delivery or window capture.
        foreach ($thread in @($foregroundThread, $targetThread) |
                Select-Object -Unique) {
            if ($thread -eq 0 -or $thread -eq $currentThread) { continue }
            if (-not [RepentogxmVita3KWin32]::AttachThreadInput(
                    $currentThread, [uint32]$thread, $true)) {
                throw "AttachThreadInput failed for thread $thread."
            }
            $attachedThreads.Add([uint32]$thread)
        }
        [void][RepentogxmVita3KWin32]::ShowWindow($handle, 9)
        [void][RepentogxmVita3KWin32]::BringWindowToTop($handle)
        [void][RepentogxmVita3KWin32]::SetForegroundWindow($handle)
        [void][RepentogxmVita3KWin32]::SetActiveWindow($handle)
        [void][RepentogxmVita3KWin32]::SetFocus($handle)
    } finally {
        for ($index = $attachedThreads.Count - 1; $index -ge 0; --$index) {
            [void][RepentogxmVita3KWin32]::AttachThreadInput(
                $currentThread, $attachedThreads[$index], $false)
        }
    }
    Start-Sleep -Milliseconds 150
    if ([RepentogxmVita3KWin32]::GetForegroundWindow() -ne $handle) {
        throw 'Exact Vita3K window did not become foreground.'
    }
    if (-not [RepentogxmVita3KWin32]::HasExactThreadFocus(
            $handle, [uint32]$script:vita3kPid)) {
        throw 'Exact Vita3K GUI thread did not acquire keyboard focus.'
    }
    return $handle
}

function Capture-ExactWindow([string]$Path) {
    if (-not $CaptureWindow) { return }
    $handle = Focus-ExactWindow
    $rect = New-Object RepentogxmVita3KWin32+RECT
    if (-not [RepentogxmVita3KWin32]::GetWindowRect($handle, [ref]$rect)) {
        throw 'GetWindowRect failed.'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { throw 'Invalid Vita3K window rectangle.' }
    $bitmap = New-Object Drawing.Bitmap $width, $height
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Send-Key([byte]$VirtualKey, [byte]$ScanCode, [bool]$Down) {
    $handle = Focus-ExactWindow
    if (-not [RepentogxmVita3KWin32]::SendKeyToExactForeground(
            $handle, [uint32]$script:vita3kPid,
            $VirtualKey, $ScanCode, $Down)) {
        throw "Sending key to exact foreground Vita3K HWND failed: vk=$VirtualKey down=$Down."
    }
    if ($Down) {
        $script:heldVirtualKey = $VirtualKey
        $script:heldScanCode = $ScanCode
    } else {
        $script:heldVirtualKey = 0
        $script:heldScanCode = 0
    }
}

function Press-Key([byte]$VirtualKey, [byte]$ScanCode) {
    Send-Key $VirtualKey $ScanCode $true
    try {
        Start-Sleep -Milliseconds $ConfirmHoldMilliseconds
    } finally {
        # Match the previously proven live sequence: focus once before down,
        # then release globally without another focus transition in between.
        [RepentogxmVita3KWin32]::ReleaseGlobalKey($VirtualKey, $ScanCode)
        $script:heldVirtualKey = 0
        $script:heldScanCode = 0
    }
}

function Release-ExerciseKeys([switch]$BestEffort) {
    if ($script:exerciseHeldKeys.Count -eq 0) { return }
    if ($BestEffort) {
        foreach ($key in @($script:exerciseHeldKeys)) {
            [RepentogxmVita3KWin32]::ReleaseGlobalKey(
                [byte]$key.VirtualKey, [byte]$key.ScanCode)
        }
        $script:exerciseHeldKeys = @()
        return
    }
    try {
        $handle = Get-ExactWindow
        if ([RepentogxmVita3KWin32]::GetForegroundWindow() -ne $handle -or
                -not [RepentogxmVita3KWin32]::HasExactThreadFocus(
                    $handle, [uint32]$script:vita3kPid)) {
            throw 'Exact Vita3K window lost focus before gameplay key release.'
        }
        foreach ($key in @($script:exerciseHeldKeys)) {
            [RepentogxmVita3KWin32]::ReleaseGlobalKey(
                [byte]$key.VirtualKey, [byte]$key.ScanCode)
        }
    } catch {
        foreach ($key in @($script:exerciseHeldKeys)) {
            [RepentogxmVita3KWin32]::ReleaseGlobalKey(
                [byte]$key.VirtualKey, [byte]$key.ScanCode)
        }
        throw
    } finally {
        $script:exerciseHeldKeys = @()
    }
}

function Press-ExercisePair([int]$Index) {
    $handle = Focus-ExactWindow

    # Default Vita3K keyboard bindings: left stick W/D/S/A, right stick I/L/K/J.
    # One movement and one shooting direction are held together, then both are
    # released before the next pair so a stopped run cannot leave a global key
    # logically pressed.
    switch ($Index % 4) {
        0 { $keys = @(@(0x57, 0x11), @(0x49, 0x17)) } # W + I: up
        1 { $keys = @(@(0x44, 0x20), @(0x4c, 0x26)) } # D + L: right
        2 { $keys = @(@(0x53, 0x1f), @(0x4b, 0x25)) } # S + K: down
        3 { $keys = @(@(0x41, 0x1e), @(0x4a, 0x24)) } # A + J: left
    }
    $held = @()
    foreach ($key in $keys) {
        if (-not [RepentogxmVita3KWin32]::SendKeyToExactForeground(
                $handle, [uint32]$script:vita3kPid,
                [byte]$key[0], [byte]$key[1], $true)) {
            foreach ($posted in $held) {
                [RepentogxmVita3KWin32]::ReleaseGlobalKey(
                    [byte]$posted.VirtualKey, [byte]$posted.ScanCode)
            }
            throw "Sending gameplay key to exact Vita3K HWND failed: vk=$($key[0])."
        }
        $held += [pscustomobject]@{
            VirtualKey = [byte]$key[0]
            ScanCode = [byte]$key[1]
        }
    }
    $script:exerciseHeldKeys = $held
    ++$script:exercisePulseCount
}

function Read-Stdout {
    if (-not (Test-Path -LiteralPath $stdoutPath)) { return '' }
    # Windows PowerShell can surface more than one pipeline object when the
    # redirected file grows during Get-Content.  Normalize those read chunks
    # here so every typed downstream parameter receives exactly one string.
    $chunks = @(Get-Content -LiteralPath $stdoutPath -Raw `
        -ErrorAction SilentlyContinue)
    if ($chunks.Count -eq 0) { return '' }
    return [string]::Concat([object[]]$chunks)
}

function Read-PresentHeartbeat([string]$Output) {
    $matches = [regex]::Matches(
        $Output, '\[kage-vita\] present heartbeat count=([0-9]+)')
    if ($matches.Count -eq 0) { return [uint64]0 }
    return [uint64]$matches[$matches.Count - 1].Groups[1].Value
}

function Get-SoakTerminalFailure {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Output,
        [Parameter(Mandatory = $true)]
        [int]$CurrentRunOutputStart,
        [Parameter(Mandatory = $true)]
        [bool]$PresentStalled,
        [Parameter(Mandatory = $true)]
        [int]$PresentStallSeconds,
        [Parameter(Mandatory = $true)]
        [uint64]$LastPresentCount
    )

    if ($CurrentRunOutputStart -lt 0) {
        throw 'Current-run output start cannot be negative.'
    }
    # A shorter file means RedirectStandardOutput replaced/truncated it after
    # the boundary was sampled; in that case the whole replacement is current.
    $effectiveStart = if ($CurrentRunOutputStart -le $Output.Length) {
        $CurrentRunOutputStart
    } else {
        0
    }
    $currentRunOutput = $Output.Substring($effectiveStart)

    # The guest-stop record is the primary fault.  KAGE writes the fuller
    # boundary comparison immediately afterwards, but neither may be hidden by
    # the later absence of present heartbeats.
    $guestStop = [regex]::Match(
        $currentRunOutput,
        '(?m)\[[^\]\r\n]+\]\s+(guest stop:[^\r\n]*)')
    if ($guestStop.Success) {
        return [pscustomobject]@{
            Kind = 'guest-stop'
            Message = "Primary guest fault: $($guestStop.Groups[1].Value.Trim())"
        }
    }
    $boundaryFail = [regex]::Match(
        $currentRunOutput,
        '(?m)\[[^\]\r\n]+\]\s+(KAGE BOUNDARY FAIL:[^\r\n]*)')
    if ($boundaryFail.Success) {
        return [pscustomobject]@{
            Kind = 'boundary-fail'
            Message = "Primary guest fault: $($boundaryFail.Groups[1].Value.Trim())"
        }
    }
    if ($PresentStalled) {
        return [pscustomobject]@{
            Kind = 'present-stall'
            Message = "No present heartbeat advanced for $PresentStallSeconds seconds (last count $LastPresentCount)."
        }
    }
    return $null
}

function Read-Vita3KExitCode {
    if (-not $script:vita3kProcess -or
            -not $script:vita3kProcess.HasExited) {
        return $null
    }
    # Start-Process may expose HasExited before its redirected stream handlers
    # have completed.  Waiting here is immediate and makes ExitCode reliable.
    $script:vita3kProcess.WaitForExit()
    $script:vita3kExitCode = [int]$script:vita3kProcess.ExitCode
    return $script:vita3kExitCode
}

function Invoke-StallDumpCapture {
    if (-not $resolvedStallDebugger) { return }

    $script:vita3kProcess.Refresh()
    if ($script:vita3kProcess.HasExited) {
        throw 'Vita3K exited before the stall dump could be captured.'
    }

    # A command file preserves quoted paths containing spaces.  Passing this
    # command through PowerShell's native argv quoting instead silently strips
    # the quotes and can make cdb dump to the wrong path.
    @(
        '.echo REPENTOGXM_STALL_CAPTURE_BEGIN'
        '.time'
        '~* r'
        '~* kv 40'
        ('.dump /ma /o "' + $stallDumpPath + '"')
        '.echo REPENTOGXM_STALL_CAPTURE_END'
        'qd'
    ) | Set-Content -LiteralPath $stallCdbCommands -Encoding ASCII

    & $resolvedStallDebugger -p $script:vita3kPid -logo $stallCdbLog `
        -cf $stallCdbCommands *> $stallCdbStdout
    $debuggerExit = $LASTEXITCODE
    if ($debuggerExit -ne 0) {
        throw "Stall debugger exited with code $debuggerExit."
    }

    $script:vita3kProcess.Refresh()
    if ($script:vita3kProcess.HasExited) {
        throw 'Stall debugger did not detach cleanly from Vita3K.'
    }
    if (-not (Test-Path -LiteralPath $stallDumpPath -PathType Leaf) -or
            (Get-Item -LiteralPath $stallDumpPath).Length -eq 0) {
        throw 'Stall debugger reported success but produced no dump.'
    }
    $script:stallDumpStatus = 'captured'
}

function Rotate-GameLogForRun {
    if (-not (Test-Path -LiteralPath $gameLogPath -PathType Leaf)) { return }
    if (Test-Path -LiteralPath $gameLogPreRun) {
        throw "Refusing to overwrite prior game-log evidence: $gameLogPreRun"
    }
    Move-Item -LiteralPath $gameLogPath -Destination $gameLogPreRun
    $script:gameLogRotation = 'rotated'
}

function Finalize-GameLogEvidence {
    if ($script:vita3kProcess -and
            (Test-Path -LiteralPath $gameLogPath -PathType Leaf)) {
        Copy-Item -LiteralPath $gameLogPath -Destination $gameLogCurrent -Force
        $script:gameLogRotation = 'captured-current'
    } elseif ($script:gameLogRotation -eq 'rotated') {
        # A boot that never recreated log.txt must not strand the user's prior
        # diagnostic outside the normal save tree.
        Move-Item -LiteralPath $gameLogPreRun -Destination $gameLogPath
        $script:gameLogRotation = 'restored-no-current'
    }
}

function Rotate-NativeLogForRun {
    if (-not (Test-Path -LiteralPath $nativeLogPath -PathType Leaf)) { return }
    if (Test-Path -LiteralPath $nativeLogPreRun) {
        throw "Refusing to overwrite prior native-log evidence: $nativeLogPreRun"
    }
    Move-Item -LiteralPath $nativeLogPath -Destination $nativeLogPreRun
    $script:nativeLogRotation = 'rotated'
}

function Finalize-NativeLogEvidence {
    if (Test-Path -LiteralPath $nativeLogPath -PathType Leaf) {
        Copy-Item -LiteralPath $nativeLogPath `
            -Destination $nativeLogCurrent -Force
        $script:nativeLogRotation = 'captured-current'
    } elseif ($script:nativeLogRotation -eq 'rotated') {
        # A boot that never recreated the native log must not strand the prior
        # diagnostic outside the title's normal data tree.
        Move-Item -LiteralPath $nativeLogPreRun -Destination $nativeLogPath
        $script:nativeLogRotation = 'restored-no-current'
    }
}

try {
    Rotate-GameLogForRun
    Rotate-NativeLogForRun
    $startArguments = @{
        FilePath = $resolvedExe
        WorkingDirectory = $workingDir
        ArgumentList = @('-l', '2', '-r', $TitleId)
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
        PassThru = $true
    }
    $script:vita3kProcess = Start-Process @startArguments
    $script:vita3kPid = $script:vita3kProcess.Id
    Write-Host "Vita3K PID=$script:vita3kPid eboot=$installedHash"

    $deadline = (Get-Date).AddSeconds($BootTimeoutSeconds)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        $script:vita3kProcess.Refresh()
        if ($script:vita3kProcess.HasExited) {
            $exitCode = Read-Vita3KExitCode
            throw "Vita3K exited during boot with code $exitCode."
        }
        $output = Read-Stdout
        $ready = ($script:vita3kProcess.MainWindowHandle -ne [IntPtr]::Zero -and
            $output -match '\[kage-vita\] first present complete')
        if ($ready) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw "$BootTimeoutSeconds-second first-present timeout." }

    Start-Sleep -Seconds $MenuInputDelaySeconds
    Capture-ExactWindow $beforeImage

    if ($AllowFocusClickFallback -and -not $NoMenuInput) {
        $handle = Get-ExactWindow
        if (-not [RepentogxmVita3KWin32]::ClickWindowForForeground(
                $handle, [uint32]$script:vita3kPid)) {
            throw 'Exact Vita3K client-focus prime failed.'
        }
        ++$script:inputClientPrimeCount
        Start-Sleep -Milliseconds 150
        if ([RepentogxmVita3KWin32]::GetForegroundWindow() -ne $handle) {
            throw 'Exact Vita3K client-focus prime lost foreground ownership.'
        }
    }

    if (-not $NoMenuInput) {
        for ($index = 1; $index -le $StartPressCount; ++$index) {
            Press-Key $StartVirtualKey $StartScanCode
            Start-Sleep -Milliseconds $ConfirmGapMilliseconds
        }
        for ($index = 1; $index -le $ConfirmCount; ++$index) {
            Press-Key $ConfirmVirtualKey $ConfirmScanCode
            if ($index -ne $ConfirmCount) {
                Start-Sleep -Milliseconds $ConfirmGapMilliseconds
            }
        }
    }

    $soakDeadline = (Get-Date).AddSeconds($SoakSeconds)
    $lastPresentAdvance = Get-Date
    $lastPresentCount = Read-PresentHeartbeat (Read-Stdout)
    $script:lastObservedPresentCount = $lastPresentCount
    $nextWatchdogPoll = Get-Date
    $exerciseIndex = 0
    $nextExerciseTransition = if ($ExerciseGameplay) {
        [void](Focus-ExactWindow)
        (Get-Date).AddSeconds($ExerciseStartDelaySeconds)
    } else {
        [datetime]::MaxValue
    }
    while ((Get-Date) -lt $soakDeadline) {
        $script:vita3kProcess.Refresh()
        $processExited = $script:vita3kProcess.HasExited
        $exitCode = if ($processExited) { Read-Vita3KExitCode } else { $null }
        $output = Read-Stdout
        $now = Get-Date
        $presentStalled = $false
        if (-not $processExited -and $now -ge $nextWatchdogPoll) {
            $presentCount = Read-PresentHeartbeat $output
            if ($presentCount -gt $lastPresentCount) {
                $lastPresentCount = $presentCount
                $script:lastObservedPresentCount = $presentCount
                $lastPresentAdvance = $now
            } elseif (($now - $lastPresentAdvance).TotalSeconds -ge
                    $PresentStallSeconds) {
                $presentStalled = $true
            }
            $nextWatchdogPoll = $now.AddSeconds(2)
        }
        $terminalFailure = Get-SoakTerminalFailure `
            -Output $output `
            -CurrentRunOutputStart $script:currentRunOutputStart `
            -PresentStalled $presentStalled `
            -PresentStallSeconds $PresentStallSeconds `
            -LastPresentCount $lastPresentCount
        if ($terminalFailure) {
            if ($terminalFailure.Kind -eq 'present-stall') {
                if ($resolvedStallDebugger) {
                    try {
                        Invoke-StallDumpCapture
                    } catch {
                        $script:stallDumpStatus = 'failed'
                        $script:stallDumpFailure = $_.Exception.Message
                    }
                }
                if ($CaptureWindow) {
                    try {
                        Capture-ExactWindow $stallImage
                    } catch {
                        $script:stallWindowFailure = $_.Exception.Message
                    }
                }
                throw "No present heartbeat advanced for $PresentStallSeconds seconds (last count $lastPresentCount)."
            }
            throw $terminalFailure.Message
        }
        if ($processExited) {
            throw "Vita3K exited during soak with code $exitCode."
        }
        if ($ExerciseGameplay -and $now -ge $nextExerciseTransition) {
            if ($script:exerciseHeldKeys.Count -ne 0) {
                Release-ExerciseKeys
                $nextExerciseTransition = $now.AddMilliseconds(
                    $ExerciseGapMilliseconds)
            } else {
                Press-ExercisePair $exerciseIndex
                ++$exerciseIndex
                $nextExerciseTransition = $now.AddMilliseconds(
                    $ExerciseHoldMilliseconds)
            }
        }
        Start-Sleep -Milliseconds 250
    }

    Capture-ExactWindow $afterImage
} catch {
    $script:capturedFailure = $_
} finally {
    Release-ExerciseKeys -BestEffort
    if ($script:heldVirtualKey) {
        [RepentogxmVita3KWin32]::ReleaseGlobalKey(
            [byte]$script:heldVirtualKey, [byte]$script:heldScanCode)
        $script:heldVirtualKey = 0
        $script:heldScanCode = 0
    }
    # Freeze the diagnostic phase before asking Vita3K to close.  Emulator
    # GUI/GXM cleanup after our explicit stop is preserved separately, but it
    # must not be attributed to guest execution during the soak.
    $script:preStopOutput = Read-Stdout
    if (Test-Path -LiteralPath $vita3kLiveLog) {
        Copy-Item -LiteralPath $vita3kLiveLog -Destination $vita3kSnapshot -Force
    }
    if ($script:vita3kProcess) {
        $script:vita3kProcess.Refresh()
        if (-not $script:vita3kProcess.HasExited) {
            [void]$script:vita3kProcess.CloseMainWindow()
            if (-not $script:vita3kProcess.WaitForExit(5000)) {
                Stop-Process -Id $script:vita3kPid -Force
                $script:vita3kProcess.WaitForExit()
            }
        }
        if ($script:vita3kProcess.HasExited) {
            [void](Read-Vita3KExitCode)
        }
    }
    Finalize-GameLogEvidence
    Finalize-NativeLogEvidence
}

$finalOutput = Read-Stdout
$preStopOutput = $script:preStopOutput
$isaacRuntimeHealth = $null
$isaacRuntimeHealthFailure = $null
if ($RequireIsaacRuntimeHealth -or $RequireIsaacFloorLifetime) {
    # Only the stdout snapshot taken before our stop request and the two files
    # rotated away before launch are attributable to this run.  Never feed the
    # pre-run evidence or the user's save backup into the health contract.
    $currentNativeEvidence = if (
        Test-Path -LiteralPath $nativeLogCurrent -PathType Leaf) {
        [IO.File]::ReadAllText($nativeLogCurrent)
    } else { '' }
    $currentGameEvidence = if (
        Test-Path -LiteralPath $gameLogCurrent -PathType Leaf) {
        [IO.File]::ReadAllText($gameLogCurrent)
    } else { '' }
    $healthContractInput = @{
        CurrentStdout = $preStopOutput
        CurrentNativeLog = $currentNativeEvidence
        CurrentGameLog = $currentGameEvidence
        Required = $true
        FloorLifeRequired = [bool]$RequireIsaacFloorLifetime
    }
    $isaacRuntimeHealth = Get-IsaacRuntimeHealthContract @healthContractInput
    if (-not $isaacRuntimeHealth.Passed) {
        $isaacRuntimeHealthFailure = $isaacRuntimeHealth.Failure
    }
}
$patterns = [ordered]@{
    first_present = '\[kage-vita\] first present complete'
    present_heartbeat = '\[kage-vita\] present heartbeat count='
    stage_heartbeat = '\[kage-vita\] stage heartbeat loop='
    audio_init = 'KAGE VITA AUDIO INIT'
    input = '\[kage-vita-input\]'
    io_profile = 'KAGE VITA IO PROFILE'
    stall_probe = '\[kage-vita-stall\]'
    glfw_attribution = 'GLFW_ERROR_ATTR'
    guest_stop = 'guest stop'
    boundary_fail = 'KAGE BOUNDARY FAIL'
}
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("title_id=$TitleId")
$summary.Add("eboot_sha256=$installedHash")
if ($openALAudioContractRequired) {
    $summary.Add("openal_config_sha256=$installedOpenALConfigHash")
}
$summary.Add("pid=$script:vita3kPid")
$summary.Add("process_exit_code=$($script:vita3kExitCode)")
$summary.Add("save_backup=$saveBackupResult")
$summary.Add("game_log_rotation=$gameLogRotation")
$summary.Add("native_log_rotation=$nativeLogRotation")
if (Test-Path -LiteralPath $gameLogPreRun -PathType Leaf) {
    $summary.Add("game_log_pre_run_sha256=$((Get-FileHash -LiteralPath $gameLogPreRun -Algorithm SHA256).Hash)")
}
if (Test-Path -LiteralPath $gameLogCurrent -PathType Leaf) {
    $gameLogCurrentItem = Get-Item -LiteralPath $gameLogCurrent
    $summary.Add("game_log_current_bytes=$($gameLogCurrentItem.Length)")
    $summary.Add("game_log_current_sha256=$((Get-FileHash -LiteralPath $gameLogCurrent -Algorithm SHA256).Hash)")
}
if (Test-Path -LiteralPath $nativeLogPreRun -PathType Leaf) {
    $summary.Add("native_log_pre_run_sha256=$((Get-FileHash -LiteralPath $nativeLogPreRun -Algorithm SHA256).Hash)")
}
if (Test-Path -LiteralPath $nativeLogCurrent -PathType Leaf) {
    $nativeLogCurrentItem = Get-Item -LiteralPath $nativeLogCurrent
    $summary.Add("native_log_current_bytes=$($nativeLogCurrentItem.Length)")
    $summary.Add("native_log_current_sha256=$((Get-FileHash -LiteralPath $nativeLogCurrent -Algorithm SHA256).Hash)")
}
if ($RequireIsaacRuntimeHealth -or $RequireIsaacFloorLifetime) {
    $healthCodes = if ($isaacRuntimeHealth.FailureCount -eq 0) {
        'none'
    } else {
        $isaacRuntimeHealth.FailureCodes -join ','
    }
    $summary.Add('isaac_runtime_health_required=True')
    $summary.Add("isaac_floor_lifetime_required=$([bool]$RequireIsaacFloorLifetime)")
    $summary.Add("isaac_floor_lifetime_present=$($isaacRuntimeHealth.FloorLifePresent)")
    $summary.Add("isaac_runtime_health_stdout_chars=$($isaacRuntimeHealth.StdoutChars)")
    $summary.Add("isaac_runtime_health_native_chars=$($isaacRuntimeHealth.NativeLogChars)")
    $summary.Add("isaac_runtime_health_game_chars=$($isaacRuntimeHealth.GameLogChars)")
    $summary.Add("isaac_runtime_health_heap_failure_records=$($isaacRuntimeHealth.HeapAllocationFailureRecords)")
    $summary.Add("isaac_runtime_health_heapovf_records=$($isaacRuntimeHealth.HeapOverflowRecords)")
    $summary.Add("isaac_runtime_health_roomslab_records=$($isaacRuntimeHealth.RoomSlabRecords)")
    $summary.Add("isaac_runtime_health_roomslabx_records=$($isaacRuntimeHealth.RoomSlabExternalRecords)")
    $summary.Add("isaac_runtime_health_stagemem_all_source_records=$($isaacRuntimeHealth.StageMemoryAllSourceRecords)")
    $summary.Add("isaac_runtime_health_stagemem_authoritative_source=$($isaacRuntimeHealth.StageMemoryAuthoritativeSource)")
    $summary.Add("isaac_runtime_health_stagemem_authoritative_records=$($isaacRuntimeHealth.StageMemoryAuthoritativeRecords)")
    $summary.Add("isaac_runtime_health_stagemem_level_pairs=$($isaacRuntimeHealth.StageMemoryLevelPairs)")
    $summary.Add("isaac_runtime_health_stagemem_load_success_pairs=$($isaacRuntimeHealth.StageMemoryLoadSuccessPairs)")
    $summary.Add("isaac_runtime_health_stagemem_load_failure_pairs=$($isaacRuntimeHealth.StageMemoryLoadFailurePairs)")
    $summary.Add("isaac_runtime_health_stagemem_reuse_records=$($isaacRuntimeHealth.StageMemoryReuseRecords)")
    $summary.Add("isaac_runtime_health_floorlife_all_source_records=$($isaacRuntimeHealth.FloorLifeAllSourceRecords)")
    $summary.Add("isaac_runtime_health_floorlife_authoritative_source=$($isaacRuntimeHealth.FloorLifeAuthoritativeSource)")
    $summary.Add("isaac_runtime_health_floorlife_authoritative_records=$($isaacRuntimeHealth.FloorLifeAuthoritativeRecords)")
    $summary.Add("isaac_runtime_health_floorlife_epoch=$($isaacRuntimeHealth.FloorLifeEpoch)")
    $summary.Add("isaac_runtime_health_floorlife_bootstraps=$($isaacRuntimeHealth.FloorLifeBootstraps)")
    $summary.Add("isaac_runtime_health_floorlife_rollovers=$($isaacRuntimeHealth.FloorLifeRollovers)")
    $summary.Add("isaac_runtime_health_floorlife_phase=$($isaacRuntimeHealth.FloorLifePhase)")
    $summary.Add("isaac_runtime_health_openal_pool_records=$($isaacRuntimeHealth.OpenALPoolRecords)")
    $summary.Add("isaac_runtime_health_openal_a005_records=$($isaacRuntimeHealth.OpenALA005Records)")
    $summary.Add("isaac_runtime_health_openal_40965_records=$($isaacRuntimeHealth.OpenALError40965Records)")
    $summary.Add("isaac_runtime_health_bad_alloc_records=$($isaacRuntimeHealth.BadAllocRecords)")
    $summary.Add("isaac_runtime_health_failure_count=$($isaacRuntimeHealth.FailureCount)")
    $summary.Add("isaac_runtime_health_failure_codes=$healthCodes")
}
$summary.Add("last_present_heartbeat=$($script:lastObservedPresentCount)")
$summary.Add("gameplay_exercise=$([bool]$ExerciseGameplay)")
$summary.Add("gameplay_exercise_pulses=$($script:exercisePulseCount)")
$summary.Add('input_delivery=exact-foreground-keybd-event')
$summary.Add("focus_click_fallback_allowed=$([bool]$AllowFocusClickFallback)")
$summary.Add("focus_click_fallbacks=$($script:focusClickFallbackCount)")
$summary.Add("input_client_primes=$($script:inputClientPrimeCount)")
$summary.Add("menu_input_delay_seconds=$MenuInputDelaySeconds")
$summary.Add("input_transitions_required=$([bool]$RequireInputTransitions)")
$summary.Add("stall_dump_status=$($script:stallDumpStatus)")
if ($script:stallDumpFailure) {
    $summary.Add("stall_dump_failure=$($script:stallDumpFailure)")
}
if ($script:stallWindowFailure) {
    $summary.Add("stall_window_failure=$($script:stallWindowFailure)")
}
if (Test-Path -LiteralPath $stallDumpPath -PathType Leaf) {
    $stallDumpItem = Get-Item -LiteralPath $stallDumpPath
    $summary.Add("stall_dump_bytes=$($stallDumpItem.Length)")
    $summary.Add("stall_dump_sha256=$((Get-FileHash -LiteralPath $stallDumpPath -Algorithm SHA256).Hash)")
}
$guestStopMatch = [regex]::Match(
    $preStopOutput,
    'guest stop: run=([-0-9]+) addr=0x([0-9a-fA-F]+) fault=(.*?) return=(?:unreadable/)?0x([0-9a-fA-F]+)')
if ($guestStopMatch.Success) {
    $summary.Add("guest_run=$($guestStopMatch.Groups[1].Value)")
    $summary.Add("guest_fault_addr=0x$($guestStopMatch.Groups[2].Value)")
    $summary.Add("guest_fault=$($guestStopMatch.Groups[3].Value)")
    $summary.Add("guest_return=0x$($guestStopMatch.Groups[4].Value)")
}
$accessCounts = Get-DiagnosticAccessCounts `
    -PreStopOutput $preStopOutput -FinalOutput $finalOutput
$hostAccessViolationCount = $accessCounts.PreStopHost
$invalidArmReadCount = $accessCounts.PreStopArm
$summary.Add("host_access_violation=$hostAccessViolationCount")
$summary.Add("invalid_arm_read=$invalidArmReadCount")
$summary.Add("shutdown_host_access_violation=$($accessCounts.ShutdownHost)")
$summary.Add("shutdown_invalid_arm_read=$($accessCounts.ShutdownArm)")
$audioReadyCount = ([regex]::Matches(
    $preStopOutput,
    'KAGE VITA AUDIO INIT:\s+status=ready(?:\s|$)')).Count
$audioSourceCapacityMismatchCount = ([regex]::Matches(
    $preStopOutput,
    'KAGE VITA AUDIO INIT:\s+status=source-capacity-mismatch(?:\s|$)')).Count
$audioConfigMissingCount = ([regex]::Matches(
    $preStopOutput, '(?im)Missing file[^\r\n]*alsoft\.conf')).Count
$audioContractFailure = Get-OpenALAudioContractFailure -Output $preStopOutput -Required $openALAudioContractRequired
$inputRequested = ((-not $NoMenuInput) -and
        ($StartPressCount -gt 0 -or $ConfirmCount -gt 0)) -or
    [bool]$ExerciseGameplay
$inputTransitionCount = ([regex]::Matches(
    $preStopOutput, $patterns.input)).Count
$summary.Add("input_requested=$inputRequested")
$summary.Add("input_transitions=$inputTransitionCount")
if ($openALAudioContractRequired) {
    $summary.Add("audio_ready=$audioReadyCount")
    $summary.Add("audio_source_capacity_mismatch=$audioSourceCapacityMismatchCount")
    $summary.Add("audio_config_missing=$audioConfigMissingCount")
}
foreach ($entry in $patterns.GetEnumerator()) {
    $count = ([regex]::Matches($preStopOutput, $entry.Value)).Count
    $summary.Add("$($entry.Key)=$count")
}
if ($script:capturedFailure) {
    $summary.Add("result=FAIL")
    $summary.Add("failure=$($script:capturedFailure.Exception.Message)")
} elseif ($preStopOutput -match 'guest stop|KAGE BOUNDARY FAIL') {
    $summary.Add('result=FAIL')
    $summary.Add('failure=guest stop or frozen boundary failure was logged')
} elseif ($hostAccessViolationCount -ne 0 -or $invalidArmReadCount -ne 0) {
    $summary.Add('result=FAIL')
    $summary.Add('failure=native access violation or invalid ARM read was logged')
} elseif ($isaacRuntimeHealthFailure) {
    $summary.Add('result=FAIL')
    $summary.Add("failure=$isaacRuntimeHealthFailure")
} elseif ($audioContractFailure) {
    $summary.Add('result=FAIL')
    $summary.Add("failure=$audioContractFailure")
} elseif ($RequireInputTransitions -and $inputTransitionCount -eq 0) {
    $summary.Add('result=FAIL')
    $summary.Add('failure=requested input produced no Vita controller transition')
} else {
    $summary.Add('result=PASS')
}
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | ForEach-Object { Write-Host $_ }

if ($script:capturedFailure -or
        $preStopOutput -match 'guest stop|KAGE BOUNDARY FAIL' -or
        $hostAccessViolationCount -ne 0 -or $invalidArmReadCount -ne 0) {
    exit 1
}
if ($audioContractFailure) { exit 1 }
if ($isaacRuntimeHealthFailure) { exit 1 }
if ($RequireInputTransitions -and $inputTransitionCount -eq 0) { exit 1 }
exit 0
