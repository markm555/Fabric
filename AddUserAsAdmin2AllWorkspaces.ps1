# Prereqs (run once)
# Install-Module MicrosoftPowerBIMgmt -Scope CurrentUser -Force
Import-Module MicrosoftPowerBIMgmt.Profile
Import-Module MicrosoftPowerBIMgmt.Admin
Import-Module MicrosoftPowerBIMgmt.Workspaces

# 1) Interactive login (MFA supported)
Connect-PowerBIServiceAccount

# 2) User to add as Admin
$userEmail = "fab2@MngEnvMCAP331330.onmicrosoft.com"

# 3) Admin API body for adding a user (emailAddress works for users)
$body = @{
  emailAddress = $userEmail
  groupUserAccessRight = "Admin"
} | ConvertTo-Json -Depth 5

# 4) Enumerate tenant workspaces (Admin scope) and apply
$workspaces = Get-PowerBIWorkspace -Scope Organization -All |
  Where-Object { $_.State -eq "Active" -and $_.Type -eq "Workspace" }

Write-Host "Found $($workspaces.Count) active workspaces."

foreach ($ws in $workspaces) {
  $url = "https://api.powerbi.com/v1.0/myorg/admin/groups/$($ws.Id)/users"
  try {
    Invoke-PowerBIRestMethod -Method POST -Url $url -Body $body | Out-Null
    Write-Host "Added $userEmail as Admin to: $($ws.Name) ($($ws.Id))"
  }
  catch {
    Write-Host "FAILED: $($ws.Name) ($($ws.Id)) :: $($_.Exception.Message)" -ForegroundColor Red
  }

  # Optional: small throttle cushion
  Start-Sleep -Milliseconds 200
}
