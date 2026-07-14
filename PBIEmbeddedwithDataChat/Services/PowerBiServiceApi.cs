using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using AppOwnsData.Models;
using Microsoft.Extensions.Configuration;
using Microsoft.Identity.Client;
using Microsoft.PowerBI.Api;
using Microsoft.PowerBI.Api.Models;
using Microsoft.Rest;

namespace AppOwnsData.Services
{
    public class PowerBiServiceApi
    {
        private readonly Guid tenantId;
        private readonly string clientId;
        private readonly string clientSecret;

        private readonly Guid workspaceId;
        private readonly Guid reportId;

        private readonly Guid dataAgentWorkspaceId;
        private readonly Guid dataAgentId;

        private readonly string powerbiServiceApiRoot;
        private readonly string powerBiServiceApiResourceId;

        private readonly string[] powerbiDefaultScope;
        private readonly string[] fabricDefaultScope;

        public PowerBiServiceApi(IConfiguration configuration)
        {
            tenantId = Guid.Parse(configuration["AzureAd:TenantId"]);
            clientId = configuration["AzureAd:ClientId"];
            clientSecret = configuration["AzureAd:ClientSecret"];

            workspaceId = Guid.Parse(configuration["PowerBi:WorkspaceId"]);
            reportId = Guid.Parse(configuration["PowerBi:ReportId"]);

            powerbiServiceApiRoot = configuration["PowerBi:PowerBiServiceApiRoot"];
            powerBiServiceApiResourceId = configuration["PowerBi:PowerBiServiceApiResourceId"];

            powerbiDefaultScope = new string[]
            {
                powerBiServiceApiResourceId + "/.default"
            };

            /*
             * Fabric Data Agent settings.
             * If FabricDataAgent:WorkspaceId is blank or missing, this uses the same workspace as the report.
             */
            string configuredAgentWorkspaceId = configuration["FabricDataAgent:WorkspaceId"];

            dataAgentWorkspaceId = string.IsNullOrWhiteSpace(configuredAgentWorkspaceId)
                ? workspaceId
                : Guid.Parse(configuredAgentWorkspaceId);

            dataAgentId = Guid.Parse(configuration["FabricDataAgent:AgentId"]);

            /*
             * This is NOT the Power BI embed token.
             * This is an Entra access token for calling the Fabric REST API from the server.
             */
            fabricDefaultScope = new string[]
            {
                "https://api.fabric.microsoft.com/.default"
            };
        }

        private async Task<string> GetAccessTokenForScopeAsync(string[] scopes)
        {
            string tenantSpecificAuthority = "https://login.microsoftonline.com/" + tenantId;

            IConfidentialClientApplication appConfidential =
                ConfidentialClientApplicationBuilder
                    .Create(clientId)
                    .WithClientSecret(clientSecret)
                    .WithAuthority(tenantSpecificAuthority)
                    .Build();

            AuthenticationResult authResult =
                await appConfidential
                    .AcquireTokenForClient(scopes)
                    .ExecuteAsync();

            return authResult.AccessToken;
        }

        public string GetAppOnlyAccessToken()
        {
            return GetAccessTokenForScopeAsync(powerbiDefaultScope)
                .GetAwaiter()
                .GetResult();
        }

        public async Task<string> GetAppOnlyAccessTokenAsync()
        {
            return await GetAccessTokenForScopeAsync(powerbiDefaultScope);
        }

        public async Task<string> GetFabricAccessTokenAsync()
        {
            return await GetAccessTokenForScopeAsync(fabricDefaultScope);
        }

        public PowerBIClient GetPowerBiClient()
        {
            string accessToken = GetAppOnlyAccessToken();

            TokenCredentials tokenCredentials =
                new TokenCredentials(accessToken, "Bearer");

            return new PowerBIClient(
                new Uri(powerbiServiceApiRoot),
                tokenCredentials);
        }

        public async Task<ReportEmbedData> GetReportEmbeddingData()
        {
            PowerBIClient pbiClient = GetPowerBiClient();

            Report report =
                await pbiClient.Reports.GetReportInGroupAsync(workspaceId, reportId);

            string datasetId = report.DatasetId;

            IList<GenerateTokenRequestV2Dataset> datasetRequests =
                new List<GenerateTokenRequestV2Dataset>();

            datasetRequests.Add(new GenerateTokenRequestV2Dataset(datasetId));

            IList<GenerateTokenRequestV2Report> reportRequests =
                new List<GenerateTokenRequestV2Report>();

            reportRequests.Add(new GenerateTokenRequestV2Report(reportId, allowEdit: true));

            GenerateTokenRequestV2 tokenRequest =
                new GenerateTokenRequestV2
                {
                    Datasets = datasetRequests,
                    Reports = reportRequests
                };

            EmbedToken embedTokenResponse =
                pbiClient.EmbedToken.GenerateToken(tokenRequest);

            return new ReportEmbedData
            {
                ReportId = reportId.ToString(),
                EmbedUrl = report.EmbedUrl,
                Token = embedTokenResponse.Token
            };
        }

        public async Task<ReportEmbedData> GetReportEmbeddingDataWithRls(
            string userName,
            string customData)
        {
            PowerBIClient pbiClient = GetPowerBiClient();

            Report report =
                await pbiClient.Reports.GetReportInGroupAsync(workspaceId, reportId);

            string datasetId = report.DatasetId;

            IList<GenerateTokenRequestV2Dataset> datasetRequests =
                new List<GenerateTokenRequestV2Dataset>();

            datasetRequests.Add(new GenerateTokenRequestV2Dataset(datasetId));

            IList<GenerateTokenRequestV2Report> reportRequests =
                new List<GenerateTokenRequestV2Report>();

            reportRequests.Add(new GenerateTokenRequestV2Report(reportId, allowEdit: true));

            List<string> datasetList =
                new List<string>
                {
                    report.DatasetId
                };

            string[] rlsRoles = new string[] { };

            IList<EffectiveIdentity> effectiveIdentities =
                new List<EffectiveIdentity>
                {
                    new EffectiveIdentity(
                        username: userName,
                        datasets: datasetList,
                        roles: rlsRoles,
                        customData: customData)
                };

            GenerateTokenRequestV2 tokenRequest =
                new GenerateTokenRequestV2
                {
                    Datasets = datasetRequests,
                    Reports = reportRequests,
                    Identities = effectiveIdentities
                };

            EmbedToken embedTokenResponse =
                pbiClient.EmbedToken.GenerateToken(tokenRequest);

            return new ReportEmbedData
            {
                ReportId = reportId.ToString(),
                EmbedUrl = report.EmbedUrl,
                Token = embedTokenResponse.Token
            };
        }
        public async Task<string> AskDataAgentAsync(string question)
        {
            if (string.IsNullOrWhiteSpace(question))
                return "Question required.";

            string accessToken = await GetFabricAccessTokenAsync();

            string baseUrl =
                "https://" +
                "api.fabric.microsoft.com/v1/workspaces/" +
                dataAgentWorkspaceId +
                "/dataagents/" +
                dataAgentId +
                "/aiassistant/openai";

            string apiVersion = "2024-05-01-preview";

            using HttpClient httpClient = new HttpClient();

            httpClient.DefaultRequestHeaders.Authorization =
                new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", accessToken);

            httpClient.DefaultRequestHeaders.Accept.Add(
                new System.Net.Http.Headers.MediaTypeWithQualityHeaderValue("application/json"));

            httpClient.DefaultRequestHeaders.Add("ActivityId", Guid.NewGuid().ToString());

            // 1. Create assistant
            string assistantJson = await PostJsonAsync(
                httpClient,
                $"{baseUrl}/assistants?api-version={apiVersion}",
                new
                {
                    model = "not used"
                });

            string assistantId = GetJsonString(assistantJson, "id");

            if (string.IsNullOrWhiteSpace(assistantId))
                return "Unable to create assistant. Response: " + assistantJson;

            // 2. Create thread
            string threadJson = await PostJsonAsync(
                httpClient,
                $"{baseUrl}/threads?api-version={apiVersion}",
                new { });

            string threadId = GetJsonString(threadJson, "id");

            if (string.IsNullOrWhiteSpace(threadId))
                return "Unable to create thread. Response: " + threadJson;

            try
            {
                // 3. Add user message to thread
                string messageJson = await PostJsonAsync(
                    httpClient,
                    $"{baseUrl}/threads/{threadId}/messages?api-version={apiVersion}",
                    new
                    {
                        role = "user",
                        content = question
                    });

                // 4. Create run
                string runJson = await PostJsonAsync(
                    httpClient,
                    $"{baseUrl}/threads/{threadId}/runs?api-version={apiVersion}",
                    new
                    {
                        assistant_id = assistantId
                    });

                string runId = GetJsonString(runJson, "id");

                if (string.IsNullOrWhiteSpace(runId))
                    return "Unable to create run. Response: " + runJson;

                // 5. Poll run status
                string status = GetJsonString(runJson, "status");
                DateTime startTime = DateTime.UtcNow;

                while (status != "completed" &&
                       status != "failed" &&
                       status != "cancelled" &&
                       status != "requires_action")
                {
                    if ((DateTime.UtcNow - startTime).TotalSeconds > 300)
                        return "Run timed out. Last status: " + status;

                    await Task.Delay(2000);

                    string runStatusJson = await GetJsonAsync(
                        httpClient,
                        $"{baseUrl}/threads/{threadId}/runs/{runId}?api-version={apiVersion}");

                    status = GetJsonString(runStatusJson, "status");
                }

                if (status != "completed")
                    return "Run finished with status: " + status;

                // 6. Read messages
                string messagesJson = await GetJsonAsync(
                    httpClient,
                    $"{baseUrl}/threads/{threadId}/messages?api-version={apiVersion}&order=asc");

                return ExtractLastAssistantMessage(messagesJson);
            }
            finally
            {
                // 7. Delete thread
                try
                {
                    await httpClient.DeleteAsync(
                        $"{baseUrl}/threads/{threadId}?api-version={apiVersion}");
                }
                catch
                {
                    // Ignore cleanup failures.
                }
            }
        }

        private async Task<string> PostJsonAsync(HttpClient httpClient, string url, object body)
        {
            string jsonBody = System.Text.Json.JsonSerializer.Serialize(body);

            using StringContent content = new StringContent(
                jsonBody,
                System.Text.Encoding.UTF8,
                "application/json");

            HttpResponseMessage response = await httpClient.PostAsync(url, content);
            string responseText = await response.Content.ReadAsStringAsync();

            if (!response.IsSuccessStatusCode)
            {
                return "Fabric Data Agent error: " +
                       (int)response.StatusCode +
                       " " +
                       response.ReasonPhrase +
                       Environment.NewLine +
                       responseText;
            }

            return responseText;
        }

        private async Task<string> GetJsonAsync(HttpClient httpClient, string url)
        {
            HttpResponseMessage response = await httpClient.GetAsync(url);
            string responseText = await response.Content.ReadAsStringAsync();

            if (!response.IsSuccessStatusCode)
            {
                return "Fabric Data Agent error: " +
                       (int)response.StatusCode +
                       " " +
                       response.ReasonPhrase +
                       Environment.NewLine +
                       responseText;
            }

            return responseText;
        }

        private string GetJsonString(string json, string propertyName)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);

                if (doc.RootElement.TryGetProperty(propertyName, out JsonElement value))
                    return value.GetString();

                return string.Empty;
            }
            catch
            {
                return string.Empty;
            }
        }

        private string ExtractLastAssistantMessage(string messagesJson)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(messagesJson);

                if (!doc.RootElement.TryGetProperty("data", out JsonElement data) ||
                    data.ValueKind != JsonValueKind.Array)
                {
                    return messagesJson;
                }

                string lastAssistantMessage = string.Empty;

                foreach (JsonElement message in data.EnumerateArray())
                {
                    string role = "";

                    if (message.TryGetProperty("role", out JsonElement roleElement))
                        role = roleElement.GetString();

                    if (!string.Equals(role, "assistant", StringComparison.OrdinalIgnoreCase))
                        continue;

                    if (!message.TryGetProperty("content", out JsonElement contentArray) ||
                        contentArray.ValueKind != JsonValueKind.Array)
                        continue;

                    foreach (JsonElement contentItem in contentArray.EnumerateArray())
                    {
                        if (contentItem.TryGetProperty("text", out JsonElement textElement))
                        {
                            if (textElement.TryGetProperty("value", out JsonElement valueElement))
                            {
                                lastAssistantMessage = valueElement.GetString();
                            }
                        }
                    }
                }

                if (!string.IsNullOrWhiteSpace(lastAssistantMessage))
                    return lastAssistantMessage;

                return messagesJson;
            }
            catch
            {
                return messagesJson;
            }
        }

        private string ExtractAgentAnswer(string responseText)
        {
            if (string.IsNullOrWhiteSpace(responseText))
            {
                return string.Empty;
            }

            try
            {
                using JsonDocument doc = JsonDocument.Parse(responseText);

                JsonElement root = doc.RootElement;

                /*
                 * OpenAI-compatible shape:
                 * {
                 *   "choices": [
                 *     {
                 *       "message": {
                 *         "content": "answer text"
                 *       }
                 *     }
                 *   ]
                 * }
                 */
                if (root.TryGetProperty("choices", out JsonElement choices) &&
                    choices.ValueKind == JsonValueKind.Array &&
                    choices.GetArrayLength() > 0)
                {
                    JsonElement firstChoice = choices[0];

                    if (firstChoice.TryGetProperty("message", out JsonElement message) &&
                        message.TryGetProperty("content", out JsonElement messageContent))
                    {
                        return messageContent.GetString();
                    }

                    if (firstChoice.TryGetProperty("text", out JsonElement textContent))
                    {
                        return textContent.GetString();
                    }
                }

                /*
                 * Possible direct response shapes.
                 */
                if (root.TryGetProperty("answer", out JsonElement answer))
                {
                    return answer.GetString();
                }

                if (root.TryGetProperty("content", out JsonElement content))
                {
                    return content.GetString();
                }

                /*
                 * If the response shape is different, return raw JSON so we can see it.
                 */
                return responseText;
            }
            catch
            {
                /*
                 * If parsing fails, return raw text so we can debug the actual payload.
                 */
                return responseText;
            }
        }
    }
}