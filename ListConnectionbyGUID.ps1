$connectionId = "0984486e-717f-45e9-85ed-263f62b7b15c"   # <-- paste GUID from dropdown

function Invoke-FabricGet {
  param([string]$Uri,[hashtable]$Headers,[int]$MaxRetries=8)

  for ($try=0; $try -le $MaxRetries; $try++) {
    try { return Invoke-RestMethod -Method GET -Uri $Uri -Headers $Headers }
    catch {
      $resp = $_.Exception.Response
      if (-not $resp) { throw }

      $statusCode = [int]$resp.StatusCode

      $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
      $raw = $reader.ReadToEnd()
      $reader.Close()

      if ($statusCode -ne 429) {
        throw "HTTP $statusCode calling $Uri. Body: $raw"
      }

      # Retry-After header if present
      $retryAfter = $resp.Headers["Retry-After"]
      if ($retryAfter) { Start-Sleep -Seconds ([int]$retryAfter); continue }

      # Parse "blocked until ... (UTC)" message (RequestBlocked)
      try {
        $j = $raw | ConvertFrom-Json
        if ($j.message -match 'until:\s*(.+)\s*\(UTC\)') {
          $utcText = $matches[1].Trim()
          $untilUtc = [datetime]::Parse($utcText, [System.Globalization.CultureInfo]::InvariantCulture,
                          [System.Globalization.DateTimeStyles]::AssumeUniversal)
          $wait = [math]::Ceiling(($untilUtc.ToUniversalTime() - [datetime]::UtcNow).TotalSeconds)
          if ($wait -lt 1) { $wait = 1 }
          Start-Sleep -Seconds $wait
          continue
        }
      } catch {}

      Start-Sleep -Seconds ([math]::Min(120, [math]::Pow(2,$try)))
    }
  }

  throw "Exceeded retries for $Uri"
}

$resourceUrl = "https://api.fabric.microsoft.com"
$token = (Get-AzAccessToken -ResourceUrl $resourceUrl).Token
$headers = @{ Authorization = "Bearer $token"; "Content-Type"="application/json" }

#$connectionId = "0984486e-717f-45e9-85ed-263f62b7b15c"   # <-- paste GUID from dropdown
$uri = "https://api.fabric.microsoft.com/v1/connections/$connectionId"

$conn = Invoke-FabricGet -Uri $uri -Headers $headers
$conn | Select-Object id, displayName, connectivityType, gatewayId, privacyLevel,
  @{n="type";e={$_.connectionDetails.type}},
  @{n="pathOrEndpoint";e={$_.connectionDetails.path}} |
  Format-List
