
function Invoke-FabricGet {
  param(
    [Parameter(Mandatory=$true)][string]$Uri,
    [Parameter(Mandatory=$true)][hashtable]$Headers,
    [int]$MaxRetries = 8
  )

  for ($try = 0; $try -le $MaxRetries; $try++) {
    try {
      return Invoke-RestMethod -Method GET -Uri $Uri -Headers $Headers
    }
    catch {
      $resp = $_.Exception.Response
      if (-not $resp) { throw }

      $statusCode = [int]$resp.StatusCode

      # Read response body (often contains RequestBlocked JSON)
      $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
      $raw = $reader.ReadToEnd()
      $reader.Close()

      if ($statusCode -ne 429) {
        # Not throttling - rethrow with context
        throw "HTTP $statusCode calling $Uri. Body: $raw"
      }

      # 429 throttling: prefer Retry-After header if present
      $retryAfter = $resp.Headers["Retry-After"]

      if ($retryAfter) {
        $sleepSec = [int]$retryAfter
        Start-Sleep -Seconds $sleepSec
        continue
      }

      # Otherwise parse "blocked ... until: M/d/yyyy h:mm:ss tt (UTC)"
      try {
        $j = $raw | ConvertFrom-Json
        if ($j.message -match 'until:\s*(.+)\s*\(UTC\)') {
          $utcText = $matches[1].Trim()
          $untilUtc = [DateTime]::Parse($utcText, [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::AssumeUniversal)
          $wait = [Math]::Ceiling(($untilUtc.ToUniversalTime() - [DateTime]::UtcNow).TotalSeconds)
          if ($wait -lt 1) { $wait = 1 }
          Start-Sleep -Seconds $wait
          continue
        }
      } catch {
        # fall through to backoff
      }

      # Fallback exponential backoff if no headers / parse fails
      $backoff = [Math]::Min(120, [Math]::Pow(2, $try))
      Start-Sleep -Seconds $backoff
    }
  }

  throw "Exceeded MaxRetries ($MaxRetries) for $Uri due to repeated throttling (429)."
}


$resourceUrl = "https://api.fabric.microsoft.com"
$token = (Get-AzAccessToken -ResourceUrl $resourceUrl).Token

$headers = @{
  "Authorization" = "Bearer $token"
  "Content-Type"  = "application/json"
}

# Get all connections with paging
$all = @()
$baseUri = "https://api.fabric.microsoft.com/v1/connections"
$uri = $baseUri

while ($true) {
  $resp = Invoke-FabricGet -Uri $uri -Headers $headers
  if ($resp.value) { $all += $resp.value }

  if ([string]::IsNullOrEmpty($resp.continuationToken)) { break }
  $uri = "$baseUri?continuationToken=$($resp.continuationToken)"
}

# Hydrate details, throttling-aware
$details = foreach ($c in $all) {
  $detailUri = "https://api.fabric.microsoft.com/v1/connections/$($c.id)"
  $d = Invoke-FabricGet -Uri $detailUri -Headers $headers

  # small pacing delay to reduce 429s
  Start-Sleep -Milliseconds 250
  $d
}

$details | Select-Object id, displayName, connectivityType, gatewayId, privacyLevel |
  Format-Table -AutoSize

