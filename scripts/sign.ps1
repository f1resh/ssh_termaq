<#
.SYNOPSIS
  Signs files (Authenticode, SHA-256, RFC 3161 timestamp) with signtool.exe from the Windows SDK.

.DESCRIPTION
  One place for code signing, used three ways:
    - on its own:                   ./scripts/sign.ps1 dist/SSHTermaq-1.0.0-win-x64/SSHTermaq.exe
    - by scripts/build.ps1 -Sign:   SSHTermaq.exe and ssh.dll before zipping, then the installer
    - by Inno Setup (ISCC):         the installer and its uninstaller (build.ps1 passes this script as the sign tool)

  The certificate is configured in scripts/signing.json (not committed; start from scripts/signing.example.json) or
  with environment variables, which win over the file (for CI). Exactly one of these must be set:

    Certificate in the Windows store (also USB tokens from a CA, whose certificate shows up there):
      "thumbprint": "<SHA-1>"            SSHTERMAQ_SIGN_THUMBPRINT
      "machineStore": true               SSHTERMAQ_SIGN_MACHINE_STORE=1   (LocalMachine\My instead of CurrentUser\My)
    PFX file (self-signed test certificates; CAs no longer issue exportable keys):
      "pfx": "C:\\keys\\test.pfx"         SSHTERMAQ_SIGN_PFX
      password only in the environment:  SSHTERMAQ_SIGN_PFX_PASSWORD
    Azure Trusted Signing (signtool dlib; sign in with `az login` or the AZURE_* variables):
      "azureDlib": "...\\Azure.CodeSigning.Dlib.dll"   SSHTERMAQ_SIGN_AZURE_DLIB
      "azureMetadata": "...\\metadata.json"            SSHTERMAQ_SIGN_AZURE_METADATA

  Optional: "timestampUrl" / SSHTERMAQ_SIGN_TIMESTAMP_URL (default DigiCert, or Microsoft's for Azure; "none" skips
  the timestamp, for tests only), "config" path via SSHTERMAQ_SIGN_CONFIG.

.EXAMPLE
  ./scripts/sign.ps1 -Check                       # show the configuration and certificate, sign nothing
  ./scripts/sign.ps1 app.exe other.dll            # sign files
#>
param(
    [switch]$Check,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'
$CodeSigningOid = '1.3.6.1.5.5.7.3.3'

function Get-SigningConfig {
    $path = $env:SSHTERMAQ_SIGN_CONFIG
    if (-not $path) { $path = Join-Path $PSScriptRoot 'signing.json' }
    $file = $null
    if (Test-Path $path) { $file = Get-Content -Raw $path | ConvertFrom-Json }
    function pick($envName, $key) {
        $v = [Environment]::GetEnvironmentVariable($envName)
        if ($v) { return $v }
        if ($file -and $file.PSObject.Properties[$key]) { return $file.$key }
        return $null
    }
    $cfg = [pscustomobject]@{
        Source        = $(if ($file) { $path } else { 'environment' })
        Thumbprint    = pick 'SSHTERMAQ_SIGN_THUMBPRINT' 'thumbprint'
        MachineStore  = "$(pick 'SSHTERMAQ_SIGN_MACHINE_STORE' 'machineStore')" -in @('1', 'true', 'True')
        Pfx           = pick 'SSHTERMAQ_SIGN_PFX' 'pfx'
        PfxPassword   = $env:SSHTERMAQ_SIGN_PFX_PASSWORD
        AzureDlib     = pick 'SSHTERMAQ_SIGN_AZURE_DLIB' 'azureDlib'
        AzureMetadata = pick 'SSHTERMAQ_SIGN_AZURE_METADATA' 'azureMetadata'
        TimestampUrl  = pick 'SSHTERMAQ_SIGN_TIMESTAMP_URL' 'timestampUrl'
        Method        = $null
    }
    $methods = @()
    if ($cfg.Thumbprint) { $methods += 'store' }
    if ($cfg.Pfx) { $methods += 'pfx' }
    if ($cfg.AzureDlib -or $cfg.AzureMetadata) { $methods += 'azure' }
    if ($methods.Count -eq 0) {
        throw "Code signing is not configured: create $path from scripts/signing.example.json, or set " +
              'SSHTERMAQ_SIGN_THUMBPRINT, SSHTERMAQ_SIGN_PFX or SSHTERMAQ_SIGN_AZURE_DLIB/_METADATA (see docs/BUILDING.md).'
    }
    if ($methods.Count -gt 1) { throw "Code signing: set only one of thumbprint, pfx or azureDlib/azureMetadata (found: $($methods -join ', '))" }
    $cfg.Method = $methods[0]
    if (-not $cfg.TimestampUrl) {
        $cfg.TimestampUrl = $(if ($cfg.Method -eq 'azure') { 'http://timestamp.acs.microsoft.com' } else { 'http://timestamp.digicert.com' })
    }
    $cfg
}

function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Directory -Filter '10.*' -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) { throw 'signtool.exe not found: install the Windows SDK (Visual Studio Installer, "Windows 11 SDK").' }
    $found
}

function Get-SigningCertificate($cfg) {
    switch ($cfg.Method) {
        'store' {
            $store = $(if ($cfg.MachineStore) { 'LocalMachine' } else { 'CurrentUser' })
            $thumb = ($cfg.Thumbprint -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
            $cert = Get-Item "Cert:\$store\My\$thumb" -ErrorAction SilentlyContinue
            if (-not $cert) { throw "Certificate $thumb not found in $store\My (is the USB token plugged in?)" }
            if (-not $cert.HasPrivateKey) { throw "Certificate $thumb in $store\My has no private key" }
            return $cert
        }
        'pfx' {
            if (-not (Test-Path $cfg.Pfx)) { throw "PFX file not found: $($cfg.Pfx)" }
            $pw = $(if ($cfg.PfxPassword) { $cfg.PfxPassword } else { '' })
            try {
                return New-Object System.Security.Cryptography.X509Certificates.X509Certificate2((Resolve-Path $cfg.Pfx).Path, $pw)
            } catch {
                throw "Cannot open $($cfg.Pfx) (wrong SSHTERMAQ_SIGN_PFX_PASSWORD?): $($_.Exception.InnerException.Message)"
            }
        }
        'azure' {
            foreach ($f in @($cfg.AzureDlib, $cfg.AzureMetadata)) {
                if (-not $f -or -not (Test-Path $f)) { throw "Azure Trusted Signing needs both azureDlib and azureMetadata; not found: '$f'" }
            }
            return $null   # the certificate lives in Azure and is issued per signing
        }
    }
}

function Show-Configuration($cfg, $cert, $signtool) {
    Write-Host "Code signing: $($cfg.Method) ($($cfg.Source))"
    Write-Host "  signtool:  $signtool"
    if ($cert) {
        Write-Host "  signer:    $($cert.Subject)"
        Write-Host "  issuer:    $($cert.Issuer)"
        Write-Host "  valid:     $($cert.NotBefore.ToString('yyyy-MM-dd')) .. $($cert.NotAfter.ToString('yyyy-MM-dd'))"
        $eku = @($cert.Extensions | Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } |
            ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
        if ($eku.Count -and $eku -notcontains $CodeSigningOid) { throw 'The certificate is not valid for code signing (no Code Signing usage)' }
        if ($cert.NotAfter -lt (Get-Date)) { throw "The certificate expired on $($cert.NotAfter.ToString('yyyy-MM-dd'))" }
        if ($cert.Subject -eq $cert.Issuer) { Write-Warning 'Self-signed certificate: fine for testing, but Windows will not trust the signature.' }
    } else {
        Write-Host "  dlib:      $($cfg.AzureDlib)"
        Write-Host "  metadata:  $($cfg.AzureMetadata)"
    }
    if ($cfg.TimestampUrl -eq 'none') {
        Write-Warning 'No timestamp: the signature becomes invalid when the certificate expires. For tests only.'
    } else {
        Write-Host "  timestamp: $($cfg.TimestampUrl)"
    }
}

$cfg = Get-SigningConfig
$signtool = Find-SignTool
$cert = Get-SigningCertificate $cfg
Show-Configuration $cfg $cert $signtool
if ($Check) { return }
if (-not $Files) { throw 'Nothing to sign: pass the files to sign (or -Check).' }

$paths = @(foreach ($f in $Files) {   # @(): one file must stay an array, or 5.1 splats its characters
    if (-not (Test-Path -LiteralPath $f)) { throw "File not found: $f" }
    (Resolve-Path -LiteralPath $f).Path
})

$signArgs = @('sign', '/fd', 'SHA256', '/d', 'SSHTermaq')
switch ($cfg.Method) {
    'store' {
        $signArgs += @('/sha1', $cert.Thumbprint)
        if ($cfg.MachineStore) { $signArgs += '/sm' }
    }
    'pfx' {
        $signArgs += @('/f', (Resolve-Path $cfg.Pfx).Path)
        if ($cfg.PfxPassword) { $signArgs += @('/p', $cfg.PfxPassword) }
    }
    'azure' { $signArgs += @('/dlib', $cfg.AzureDlib, '/dmdf', $cfg.AzureMetadata) }
}
if ($cfg.TimestampUrl -ne 'none') { $signArgs += @('/tr', $cfg.TimestampUrl, '/td', 'SHA256') }

# Timestamp servers fail now and then: retry a few times before giving up.
$shown = ($signArgs -join ' ')
if ($cfg.PfxPassword) { $shown = $shown.Replace($cfg.PfxPassword, '***') }
for ($attempt = 1; ; $attempt++) {
    $ErrorActionPreference = 'Continue'   # Windows PowerShell 5.1 would stop on signtool's stderr
    $out = & $signtool @signArgs @paths 2>&1
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -eq 0) { break }
    if ($attempt -ge 3) {
        $out | Out-Host
        throw "signtool failed (exit $LASTEXITCODE): signtool $shown"
    }
    Write-Warning "signtool failed (attempt $attempt of 3), retrying in 5 s"
    Start-Sleep -Seconds 5
}

foreach ($p in $paths) {
    $sig = Get-AuthenticodeSignature -LiteralPath $p
    if (-not $sig.SignerCertificate) { throw "Not signed after signing: $p ($($sig.Status))" }
    $trust = $(if ($sig.Status -eq 'Valid') { 'trusted' } else { "not trusted here: $($sig.Status)" })
    $ts = $(if ($sig.TimeStamperCertificate) { 'timestamped' } else { 'no timestamp' })
    Write-Host ("Signed {0} ({1}, {2})" -f (Split-Path $p -Leaf), $trust, $ts) -ForegroundColor Green
}
