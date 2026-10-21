#Requires -Modules Az.Accounts, Az.Resources
import-module az.accounts
import-module az.resources

$CapacityName = "markmf2"

# ------------------------------------------------------------
# LOGIN
# ------------------------------------------------------------

$TenantId = "9a4352fe-a85b-4e5e-88f6-8b368c03c88a"

$Credential = Get-AutomationPSCredential -Name "Fabmin"

Connect-AzAccount `
    -ServicePrincipal `
    -Tenant $TenantId `
    -Credential $Credential

#$SecureSecret = ConvertTo-SecureString $ClientSecret -AsPlainText -Force
#$Credential = New-Object System.Management.Automation.PSCredential(
#    $ClientId,
#    $SecureSecret
#)

Connect-AzAccount `
    -ServicePrincipal `
    -Tenant $TenantId `
    -Credential $Credential

$ctx = Get-AzContext

if (-not $ctx) {
    throw "No Azure context available."
}

$SubscriptionId = $ctx.Subscription.Id

Write-Host "Subscription: $SubscriptionId" -ForegroundColor Cyan
Write-Host "Looking for Fabric capacity '$CapacityName'..." -ForegroundColor Cyan


# ------------------------------------------------------------
# FIND FABRIC CAPACITY
# ------------------------------------------------------------

$capacity = Get-AzResource `
    -ResourceType "Microsoft.Fabric/capacities" |
    Where-Object { $_.Name -eq $CapacityName }

if (-not $capacity) {
    throw "Fabric capacity '$CapacityName' was not found."
}

$ResourceGroupName = $capacity.ResourceGroupName

Write-Host "Found capacity:" -ForegroundColor Green
Write-Host "  Capacity:       $CapacityName"
Write-Host "  Resource Group: $ResourceGroupName"


# ------------------------------------------------------------
# GET ARM TOKEN
# ------------------------------------------------------------

#$armToken = (Get-AzAccessToken `
#    -ResourceUrl "https://management.azure.com").Token

#$armHeaders = @{
#    Authorization = "Bearer $armToken"
#}

$secureToken = (Get-AzAccessToken `
    -ResourceUrl "https://management.azure.com/").Token

$armToken = [System.Net.NetworkCredential]::new(
    "",
    $secureToken
).Password

$armHeaders = @{
    Authorization = "Bearer $armToken"
    "Content-Type" = "application/json"
}

# ------------------------------------------------------------
# RESUME CAPACITY
# ------------------------------------------------------------

$resumeUri = `
"https://management.azure.com/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroupName/providers/Microsoft.Fabric/capacities/$CapacityName/resume?api-version=2023-11-01"

Write-Host ""
Write-Host "Resuming capacity '$CapacityName'..." -ForegroundColor Yellow

try {
    Invoke-RestMethod `
        -Method POST `
        -Uri $resumeUri `
        -Headers $armHeaders

    Write-Host "Resume request submitted." -ForegroundColor Green
}
catch {
    # A capacity already running may return an error depending on state.
    Write-Host "Resume returned: $($_.Exception.Message)" -ForegroundColor Yellow
}


# ------------------------------------------------------------
# WAIT FOR CAPACITY TO BECOME ACTIVE
# ------------------------------------------------------------

$statusUri = `
"https://management.azure.com/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroupName/providers/Microsoft.Fabric/capacities/$($CapacityName)?api-version=2023-11-01"

$statusUri
Write-Host "Waiting for Fabric capacity to become Active..." -ForegroundColor Cyan

$timeout = 300
$elapsed = 0

do {

    Start-Sleep -Seconds 10
    $elapsed += 10

    $capacityStatus = Invoke-RestMethod `
        -Method GET `
        -Uri $statusUri `
        -Headers $armHeaders

    $state = $capacityStatus.properties.state

    Write-Host "  Capacity state: $state"

    if ($elapsed -ge $timeout) {
        throw "Timed out waiting for Fabric capacity to become active."
    }

} until ($state -eq "Active")


Write-Host ""
Write-Host "Capacity '$CapacityName' is ACTIVE." -ForegroundColor Green


# ------------------------------------------------------------
# GET FABRIC TOKEN
# ------------------------------------------------------------

#$fabricToken = (Get-AzAccessToken `
#    -ResourceUrl "https://api.fabric.microsoft.com").Token

#$fabricHeaders = @{
#    Authorization = "Bearer $fabricToken"
#    "Content-Type" = "application/json"
#}

$secureFabricToken = (Get-AzAccessToken `
    -ResourceUrl "https://api.fabric.microsoft.com").Token

$fabricToken = [System.Net.NetworkCredential]::new(
    "",
    $secureFabricToken
).Password

$fabricHeaders = @{
    Authorization = "Bearer $fabricToken"
    "Content-Type" = "application/json"
}


# ------------------------------------------------------------
# ENUMERATE WORKSPACES
# ------------------------------------------------------------

Write-Host ""
Write-Host "Enumerating Fabric workspaces..." -ForegroundColor Cyan

$workspaceUri = "https://api.fabric.microsoft.com/v1/workspaces"

$workspaces = @()

do {

    $response = Invoke-RestMethod `
        -Method GET `
        -Uri $workspaceUri `
        -Headers $fabricHeaders

    $workspaces += $response.value

    if ($response.continuationUri) {
        $workspaceUri = $response.continuationUri
    }
    else {
        $workspaceUri = $null
    }

} while ($workspaceUri)


Write-Host "Found $($workspaces.Count) accessible workspaces."


# ------------------------------------------------------------
# FIND AND START MIRRORED DATABASES
# ------------------------------------------------------------

foreach ($workspace in $workspaces) {

    $workspaceId = $workspace.id
    $workspaceName = $workspace.displayName

    Write-Host ""
    Write-Host "Workspace: $workspaceName" -ForegroundColor Cyan

    $mirrorUri = `
        "https://api.fabric.microsoft.com/v1/workspaces/$workspaceId/mirroredDatabases"

    try {

        $mirrors = Invoke-RestMethod `
            -Method GET `
            -Uri $mirrorUri `
            -Headers $fabricHeaders

    }
    catch {

        Write-Host "  Unable to enumerate mirrored databases."
        continue
    }


    foreach ($mirror in $mirrors.value) {

        $mirrorId = $mirror.id
        $mirrorName = $mirror.displayName

        Write-Host "  Mirrored Database: $mirrorName" -ForegroundColor White


        # ----------------------------------------------------
        # GET MIRROR STATUS
        # ----------------------------------------------------

        #$mirrorStatusUri = `
        #    "https://api.fabric.microsoft.com/v1/workspaces/$workspaceId/mirroredDatabases/$mirrorId/mirroring/status"

        #try {

         #   $mirrorStatus = Invoke-RestMethod `
        #        -Method GET `
        #        -Uri $mirrorStatusUri `
        #        -Headers $fabricHeaders

        #    $status = $mirrorStatus.status

        #    Write-Host "    Status: $status"

        #}
        #catch {

        #    Write-Host "    Unable to retrieve mirroring status." `
        #        -ForegroundColor Red

        #    continue
        #}
        # ----------------------------------------------------
        # GET MIRROR STATUS
        # ----------------------------------------------------

        $mirrorStatusUri = `
            "https://api.fabric.microsoft.com/v1/workspaces/$workspaceId/mirroredDatabases/$mirrorId/getMirroringStatus"

        try {

            $mirrorStatus = Invoke-RestMethod `
                -Method POST `
                -Uri $mirrorStatusUri `
                -Headers $fabricHeaders

            $status = $mirrorStatus.status

            Write-Host "    Status: $status"

        }
        catch {

            Write-Host "    Unable to retrieve mirroring status." `
            -ForegroundColor Red

            Write-Host $_.Exception.Message `
            -ForegroundColor DarkGray

            continue
        }

        # ----------------------------------------------------
        # START MIRRORING IF NECESSARY
        # ----------------------------------------------------

        if ($status -ne "Running") {

            Write-Host "    Starting mirroring..." `
                -ForegroundColor Yellow

            $startUri = `
                "https://api.fabric.microsoft.com/v1/workspaces/$workspaceId/mirroredDatabases/$mirrorId/startMirroring"

            try {

                Invoke-RestMethod `
                    -Method POST `
                    -Uri $startUri `
                    -Headers $fabricHeaders

                Write-Host "    Mirroring started." `
                    -ForegroundColor Green
            }
            catch {

                Write-Host "    FAILED to start mirroring." `
                    -ForegroundColor Red

                Write-Host "    $($_.Exception.Message)"
            }

        }
        else {

            Write-Host "    Already running." `
                -ForegroundColor Green
        }
    }
}


Write-Host ""
Write-Host "===================================================="
Write-Host "Fabric resume / mirroring recovery complete." `
    -ForegroundColor Green
Write-Host "===================================================="
