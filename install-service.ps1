[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$InstallDriver,
    [switch]$LoadDriver,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$serviceName = 'IoMonitorService'
$displayName = 'Fast IoMonitor Broker Service'
$installDirectory = Join-Path $env:ProgramFiles 'IoMonitor'
$installedBinary = Join-Path $installDirectory 'IoMonitorService.exe'
$sourceBinary = Join-Path $root "bin\x64\$Configuration\IoMonitorService.exe"
$brokerRegistryPath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\IoMonitorService'
$driverRegistryPath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\IoMonitor'
$certificateStatePath = Join-Path $root '.iomonitor-cert-state.json'
$sc = Join-Path $env:SystemRoot 'System32\sc.exe'
$fltmc = Join-Path $env:SystemRoot 'System32\fltmc.exe'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$installDriverRequested = $InstallDriver -or $LoadDriver
$serviceControlAccessMask = 0x00000034 # QUERY_STATUS | START | STOP

function Get-IoMonitorServiceSecurityDescriptor {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $output = @(& $sc sdshow $Name 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "SC could not read the security descriptor for $Name (exit code $exitCode): $($output -join ' ')"
    }

    $sddl = ($output |
        ForEach-Object { $_.ToString().Trim() } |
        Where-Object { $_ -match '^(?:O:|G:|D:|S:)' }) -join ''
    if ([string]::IsNullOrWhiteSpace($sddl)) {
        throw "SC returned no security descriptor for $Name."
    }

    try {
        [Security.AccessControl.RawSecurityDescriptor]::new($sddl)
    } catch {
        throw "SC returned an invalid security descriptor for ${Name}: $($_.Exception.Message)"
    }
}

function Test-IoMonitorServiceAccess {
    param(
        [Parameter(Mandatory)]
        [Security.AccessControl.RawSecurityDescriptor]$Descriptor,

        [Parameter(Mandatory)]
        [Security.Principal.SecurityIdentifier]$UserSid,

        [Parameter(Mandatory)]
        [int]$RequiredAccessMask
    )

    if ($null -eq $Descriptor.DiscretionaryAcl) {
        return $false
    }

    foreach ($ace in $Descriptor.DiscretionaryAcl) {
        if ($ace -is [Security.AccessControl.CommonAce] -and
            $ace.AceQualifier -eq [Security.AccessControl.AceQualifier]::AccessAllowed -and
            $ace.SecurityIdentifier -eq $UserSid -and
            ($ace.AccessMask -band $RequiredAccessMask) -eq $RequiredAccessMask) {
            return $true
        }
    }

    return $false
}

function Grant-IoMonitorServiceControl {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [Security.Principal.SecurityIdentifier]$UserSid
    )

    try {
        $accountName = $UserSid.Translate(
            [Security.Principal.NTAccount]).Value
        $accountDisplay = $accountName
    } catch [Security.Principal.IdentityNotMappedException] {
        $accountDisplay = $UserSid.ToString()
    }

    $descriptor = Get-IoMonitorServiceSecurityDescriptor -Name $Name
    if (Test-IoMonitorServiceAccess `
            -Descriptor $descriptor `
            -UserSid $UserSid `
            -RequiredAccessMask $serviceControlAccessMask) {
        Write-Host "Service start, stop, and status rights are already granted to $accountDisplay."
        return
    }

    if ($null -eq $descriptor.DiscretionaryAcl) {
        throw "Refusing to replace the null DACL on $Name."
    }

    $newAce = [Security.AccessControl.CommonAce]::new(
        [Security.AccessControl.AceFlags]::None,
        [Security.AccessControl.AceQualifier]::AccessAllowed,
        $serviceControlAccessMask,
        $UserSid,
        $false,
        [byte[]]$null)

    $insertIndex = $descriptor.DiscretionaryAcl.Count
    for ($index = 0; $index -lt $descriptor.DiscretionaryAcl.Count; ++$index) {
        if (($descriptor.DiscretionaryAcl[$index].AceFlags -band
                [Security.AccessControl.AceFlags]::Inherited) -ne 0) {
            $insertIndex = $index
            break
        }
    }
    $descriptor.DiscretionaryAcl.InsertAce($insertIndex, $newAce)

    $updatedSddl = $descriptor.GetSddlForm(
        [Security.AccessControl.AccessControlSections]::All)
    Write-Host "Granting service start, stop, and status rights to $accountDisplay."
    & $sc sdset $Name $updatedSddl
    if ($LASTEXITCODE -ne 0) {
        throw "SC could not update the security descriptor for $Name (exit code $LASTEXITCODE)."
    }

    $verifiedDescriptor = Get-IoMonitorServiceSecurityDescriptor -Name $Name
    if (-not (Test-IoMonitorServiceAccess `
            -Descriptor $verifiedDescriptor `
            -UserSid $UserSid `
            -RequiredAccessMask $serviceControlAccessMask)) {
        throw "SC did not persist the requested service rights for $UserSid on $Name."
    }
}

function Get-IoMonitorPublishedInfNames {
    $windowsInfDirectory = Join-Path $env:SystemRoot 'INF'
    $publishedNames = foreach ($candidate in
        Get-ChildItem -LiteralPath $windowsInfDirectory -Filter 'oem*.inf' -File) {
        try {
            $content = Get-Content -LiteralPath $candidate.FullName -Raw
        } catch {
            continue
        }

        # Match several project-specific markers. The OEM file name alone is not
        # sufficient because other ActivityMonitor drivers can share this class.
        if ($content -match '(?im)^\s*ClassGuid\s*=\s*\{b86dff51-a31e-4bac-b3cf-e8cfe75c9fc2\}\s*$' -and
            $content -match '(?im)^\s*ProviderName\s*=\s*"(?:Fast IoMonitor|IoMonitor MVP)"\s*$' -and
            $content -match '(?im)^\s*ServiceName\s*=\s*"IoMonitor"\s*$' -and
            $content -match '(?im)^\s*DefaultAltitude\s*=\s*"385201"\s*$') {
            $candidate.Name
        }
    }

    @($publishedNames | Sort-Object -Unique)
}

if ($Uninstall -and $installDriverRequested) {
    throw '-Uninstall cannot be combined with -InstallDriver or -LoadDriver.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw @'
Service and driver changes require an elevated PowerShell. Run:
  sudo pwsh.exe -NoProfile -File .\install-service.ps1 -LoadDriver
'@
}

$existingService = Get-Service -Name $serviceName -ErrorAction SilentlyContinue

if ($Uninstall) {
    if ($null -ne $existingService) {
        if ($existingService.Status -ne 'Stopped') {
            Write-Host "Stopping $serviceName."
            Stop-Service -Name $serviceName -Force
            (Get-Service -Name $serviceName).WaitForStatus(
                'Stopped',
                [TimeSpan]::FromSeconds(15))
        }

        Write-Host "Deleting $serviceName."
        & $sc delete $serviceName
        if ($LASTEXITCODE -notin @(0, 1072)) {
            throw "SC could not delete $serviceName (exit code $LASTEXITCODE)."
        }
        $existingService.Dispose()
        $existingService = $null

        for ($attempt = 0; $attempt -lt 50; ++$attempt) {
            if (-not (Test-Path -LiteralPath $brokerRegistryPath)) {
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if (Test-Path -LiteralPath $brokerRegistryPath) {
            throw "$serviceName is still marked for deletion. Close open service-management tools and retry."
        }
    } else {
        Write-Host "$serviceName is not installed."
    }

    $loadedFilters = (& $fltmc filters 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "FltMC could not enumerate minifilters (exit code $LASTEXITCODE)."
    }
    if ($loadedFilters -match '(?m)^\s*IoMonitor\s') {
        Write-Host 'Unloading IoMonitor minifilter.'
        & $fltmc unload IoMonitor
        if ($LASTEXITCODE -ne 0) {
            throw @"
FltMC could not unload IoMonitor (exit code $LASTEXITCODE).
Stop every directly connected process and try again.
"@
        }
    } else {
        Write-Host 'IoMonitor minifilter is not loaded.'
    }

    $publishedInfNames = @(Get-IoMonitorPublishedInfNames)
    if ($publishedInfNames.Count -eq 0) {
        if (Test-Path -LiteralPath $driverRegistryPath) {
            throw @'
The IoMonitor driver service still exists, but its OEM INF package could not be
identified safely. No driver package was deleted.
'@
        }
        Write-Host 'No IoMonitor OEM driver package was found.'
    } else {
        foreach ($publishedInfName in $publishedInfNames) {
            if ($publishedInfName -notmatch '^oem\d+\.inf$') {
                throw "Refusing to delete an invalid published INF name: $publishedInfName"
            }

            Write-Host "Deleting IoMonitor driver package $publishedInfName."
            & $pnputil /delete-driver $publishedInfName /uninstall
            if ($LASTEXITCODE -ne 0) {
                throw "PnPUtil failed to delete $publishedInfName (exit code $LASTEXITCODE)."
            }
        }
    }

    if (Test-Path -LiteralPath $driverRegistryPath) {
        Write-Host 'Deleting the remaining IoMonitor driver service registration.'
        & $sc delete IoMonitor
        if ($LASTEXITCODE -notin @(0, 1072)) {
            throw "SC could not delete the IoMonitor driver service (exit code $LASTEXITCODE)."
        }
    }

    if (Test-Path -LiteralPath $installedBinary -PathType Leaf) {
        Remove-Item -LiteralPath $installedBinary -Force
    }
    if ((Test-Path -LiteralPath $installDirectory -PathType Container) -and
        -not (Get-ChildItem -LiteralPath $installDirectory -Force)) {
        Remove-Item -LiteralPath $installDirectory -Force
    }
    if (Test-Path -LiteralPath $certificateStatePath -PathType Leaf) {
        Write-Host 'The tracked WDK test certificate remains installed because it may be shared by other test drivers.'
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $sourceBinary -PathType Leaf)) {
    throw "Service binary not found: $sourceBinary. Run build.ps1 first."
}

if ($installDriverRequested) {
    $driverPackage = Join-Path $root "bin\x64\$Configuration\IoMonitorDriver"
    $infPath = Join-Path $driverPackage 'io_monitor.inf'
    $sysPath = Join-Path $driverPackage 'IoMonitor.sys'
    $catalogPath = Join-Path $driverPackage 'iomonitor.cat'
    $certificatePath = Join-Path $root "bin\x64\$Configuration\IoMonitor.cer"

    foreach ($artifact in @($infPath, $sysPath, $catalogPath, $certificatePath)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Required driver package artifact was not found: $artifact"
        }
    }

    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $certificatePath)
    foreach ($signedArtifact in @($sysPath, $catalogPath)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $signedArtifact
        if ($null -eq $signature.SignerCertificate) {
            throw "The driver artifact does not contain an Authenticode signer: $signedArtifact"
        }
        if ($signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
            throw @"
The exported test certificate does not match the signer of $signedArtifact.
Rebuild the driver package before installing it.
"@
        }
    }
}
elseif (-not (Test-Path -LiteralPath $driverRegistryPath)) {
    throw @'
The IoMonitor minifilter is not installed. Run this script with -InstallDriver
or -LoadDriver after building the solution.
'@
}

# Stop the broker before replacing its binary or reloading its driver connection.
if ($null -ne $existingService -and $existingService.Status -ne 'Stopped') {
    Write-Host "Stopping the existing $serviceName instance."
    Stop-Service -Name $serviceName -Force
    (Get-Service -Name $serviceName).WaitForStatus(
        'Stopped',
        [TimeSpan]::FromSeconds(15))
}

if ($installDriverRequested) {
    $rundll32 = Join-Path $env:SystemRoot 'System32\rundll32.exe'
    $certutil = Join-Path $env:SystemRoot 'System32\certutil.exe'

    Write-Warning 'Installing the WDK test certificate into system-wide trust stores. Use this only on a test system.'
    $certificateStores = @(
        @{ Name = 'Root'; Path = "Cert:\LocalMachine\Root\$($certificate.Thumbprint)" },
        @{ Name = 'TrustedPublisher'; Path = "Cert:\LocalMachine\TrustedPublisher\$($certificate.Thumbprint)" }
    )

    $previousCertificate = $null
    if (Test-Path -LiteralPath $certificateStatePath -PathType Leaf) {
        try {
            $previousCertificate = Get-Content -LiteralPath $certificateStatePath -Raw |
                ConvertFrom-Json
            if ($previousCertificate.Project -ne 'IoMonitor' -or
                $previousCertificate.Thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
                throw 'The certificate state file has an invalid format.'
            }
        } catch {
            throw "Cannot safely clean up the previous certificate: $($_.Exception.Message)"
        }
    }

    if ($null -ne $previousCertificate -and
        $previousCertificate.Thumbprint -ne $certificate.Thumbprint) {
        Write-Warning "Removing the previous IoMonitor test certificate $($previousCertificate.Thumbprint)."
        foreach ($store in $certificateStores) {
            $previousStorePath = "Cert:\LocalMachine\$($store.Name)\$($previousCertificate.Thumbprint)"
            if (Test-Path -LiteralPath $previousStorePath) {
                $storedCertificate = Get-Item -LiteralPath $previousStorePath
                if ($storedCertificate.Subject -ne $previousCertificate.Subject) {
                    throw "Refusing to remove a certificate whose subject does not match the IoMonitor state file: $previousStorePath"
                }

                & $certutil -delstore $store.Name $previousCertificate.Thumbprint
                if ($LASTEXITCODE -ne 0) {
                    throw "CertUtil failed to remove the previous certificate from LocalMachine\$($store.Name) (exit code $LASTEXITCODE)."
                }
            }
        }
    }

    foreach ($store in $certificateStores) {
        if (-not (Test-Path -LiteralPath $store.Path)) {
            & $certutil -addstore -f $store.Name $certificatePath
            if ($LASTEXITCODE -ne 0) {
                throw "CertUtil failed to add the test certificate to LocalMachine\$($store.Name) (exit code $LASTEXITCODE)."
            }
        } else {
            Write-Host "Test certificate is already present in LocalMachine\$($store.Name)."
        }
    }

    [pscustomobject]@{
        Project = 'IoMonitor'
        Thumbprint = $certificate.Thumbprint
        Subject = $certificate.Subject
        UpdatedUtc = [DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json | Set-Content -LiteralPath $certificateStatePath -Encoding utf8

    Write-Host "Installing minifilter package from $infPath"
    & $pnputil /add-driver $infPath /install
    if ($LASTEXITCODE -ne 0) {
        throw "PnPUtil failed to install the driver package (exit code $LASTEXITCODE)."
    }

    if (-not (Test-Path -LiteralPath $driverRegistryPath)) {
        Write-Host 'PnPUtil added the package but did not register the service; invoking the INF DefaultInstall section.'
        & $rundll32 setupapi.dll,InstallHinfSection DefaultInstall 132 $infPath
        if ($LASTEXITCODE -ne 0) {
            throw "SetupAPI failed to install the INF DefaultInstall section (exit code $LASTEXITCODE)."
        }
    }

    if (-not (Test-Path -LiteralPath $driverRegistryPath)) {
        throw @"
The driver package was added, but the INF installation did not register the
IoMonitor service. Check C:\Windows\INF\setupapi.dev.log for the installation
error.
"@
    }

    & $sc query IoMonitor
    if ($LASTEXITCODE -ne 0) {
        throw "The IoMonitor service registry key exists, but Service Control Manager cannot query it (exit code $LASTEXITCODE)."
    }
    Write-Host 'IoMonitor driver service is installed.'
}

if (-not (Test-Path -LiteralPath $driverRegistryPath)) {
    throw @'
The IoMonitor minifilter is not installed. Run this script with -InstallDriver
or -LoadDriver after building the solution.
'@
}

if ($LoadDriver) {
    $loadedFilters = (& $fltmc filters 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "FltMC could not enumerate minifilters (exit code $LASTEXITCODE)."
    }

    if ($loadedFilters -match '(?m)^\s*IoMonitor\s') {
        Write-Host 'Reloading the already loaded IoMonitor minifilter.'
        & $fltmc unload IoMonitor
        if ($LASTEXITCODE -ne 0) {
            throw @"
FltMC could not unload the previous IoMonitor version (exit code $LASTEXITCODE).
Stop every directly connected process and try again.
"@
        }
    }

    Write-Host 'Loading IoMonitor minifilter.'
    & $fltmc load IoMonitor
    if ($LASTEXITCODE -ne 0) {
        throw @"
FltMC could not load IoMonitor (exit code $LASTEXITCODE). If Windows reports
error 577, enable test signing in a test environment and restart Windows. Error
127 indicates that the driver imports a kernel routine unavailable on this
Windows version; rebuild the driver for the documented minimum Windows target.
"@
    }

    & $fltmc filters
    if ($LASTEXITCODE -ne 0) {
        throw "FltMC could not verify the loaded filter (exit code $LASTEXITCODE)."
    }
    & $fltmc instances -f IoMonitor
    if ($LASTEXITCODE -ne 0) {
        throw "FltMC could not enumerate IoMonitor instances (exit code $LASTEXITCODE)."
    }
}

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -LiteralPath $sourceBinary -Destination $installedBinary -Force

if ($null -eq $existingService) {
    Write-Host "Creating $serviceName."
    New-Service `
        -Name $serviceName `
        -BinaryPathName "`"$installedBinary`"" `
        -DisplayName $displayName `
        -Description 'Privileged broker for Fast IoMonitor.' `
        -StartupType Manual `
        -DependsOn 'IoMonitor' | Out-Null
} else {
    Write-Host "Updating $serviceName."
    $quotedInstalledBinary = '"' + $installedBinary + '"'
    & $sc config $serviceName `
        'binPath=' $quotedInstalledBinary `
        'start=' 'demand' `
        'depend=' 'IoMonitor' `
        'DisplayName=' $displayName
    if ($LASTEXITCODE -ne 0) {
        throw "SC could not update $serviceName (exit code $LASTEXITCODE)."
    }
    & $sc description $serviceName 'Privileged broker for Fast IoMonitor.'
    if ($LASTEXITCODE -ne 0) {
        throw "SC could not update the service description (exit code $LASTEXITCODE)."
    }
}

Grant-IoMonitorServiceControl -Name $serviceName -UserSid $identity.User

& $sc query $serviceName
if ($LASTEXITCODE -ne 0) {
    throw "SC could not verify $serviceName (exit code $LASTEXITCODE)."
}
Write-Host "$serviceName is installed and will be started by IoMonitorClient when required."
