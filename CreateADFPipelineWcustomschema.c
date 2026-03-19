using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

using Azure.Core;
using Azure.Identity;

internal class Program
{
    private const string ApiVersion = "2018-06-01";

    static async Task<int> Main(string[] args)
    {
        var o = Options.Parse(args);
        if (o == null) return 2;

        // Auth to ARM. DefaultAzureCredential works for:
        // - local dev with "az login" or VS sign-in
        // - managed identity when deployed
        var credential = new DefaultAzureCredential();
        var token = await credential.GetTokenAsync(
            new TokenRequestContext(new[] { "https://management.azure.com/.default" }));

        using var http = new HttpClient();
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token.Token);

        // Names we’ll deploy
        var lsSql = "ls_onprem_sql";
        var lsLakehouse = "ls_fabric_lakehouse";
        var dsSql = "ds_onprem_sql_table";
        var dsLakehouse = "ds_fabric_lakehouse_table";
        var pipelineName = "pl_copy_sql_to_lakehouse";

        // 1) Linked Service: On-prem SQL Server using SHIR
        // (Self-hosted IR is the bridge for on-prem connectivity) [1](https://learn.microsoft.com/en-us/azure/data-factory/concepts-integration-runtime)
        var sqlLinkedService = BuildSqlServerLinkedService(lsSql, o, o.IntegrationRuntimeName);

        await PutLinkedServiceAsync(http, o, lsSql, sqlLinkedService);

        // 2) Linked Service: Fabric Lakehouse connector (type = Lakehouse) with required properties
        // Microsoft Learn lists required properties workspaceId/artifactId/tenant/servicePrincipalId/servicePrincipalCredentialType/servicePrincipalCredential [9](https://learn.microsoft.com/en-us/azure/data-factory/connector-microsoft-fabric-lakehouse)
        var lakehouseLinkedService = BuildFabricLakehouseLinkedService(lsLakehouse, o);

        await PutLinkedServiceAsync(http, o, lsLakehouse, lakehouseLinkedService);

        // 3) Dataset: source SQL table
        var sqlDataset = BuildSqlServerTableDataset(dsSql, lsSql, o.SourceSqlTable);
        await PutDatasetAsync(http, o, dsSql, sqlDataset);

        // 4) Dataset: sink Lakehouse table WITH SCHEMA + TABLE
        // typeProperties.schema is the schema name property for Microsoft Fabric Lakehouse Table dataset [5](https://learn.microsoft.com/en-us/dotnet/api/microsoft.azure.management.datafactory.models.lakehousetabledataset.typepropertiesschema?view=az-ps-latest)
        var lakehouseDataset = BuildLakehouseTableDataset(dsLakehouse, lsLakehouse, o.TargetSchema, o.TargetTable);
        await PutDatasetAsync(http, o, dsLakehouse, lakehouseDataset);

        // 5) Pipeline: Copy activity sql -> lakehouse
        var pipeline = BuildCopyPipeline(pipelineName, dsSql, dsLakehouse);
        await PutPipelineAsync(http, o, pipelineName, pipeline);

        // 6) Run pipeline (createRun)
        // createRun is documented REST POST for ADF pipelines [7](https://teams.microsoft.com/l/meeting/details?eventId=AAMkADM0NzczMzFkLTQ2NDYtNDdmYy04MmJmLTBkZmE4OTFhMjE3YgBGAAAAAABDIuXcFUzPEYOWAIBfOF2yBwDhkCp9pmKMSphiZMPL7T9XAAlfYwABAACI_cT-xQKbSIgBs7Ed6PQVAAe-LK_qAAA%3d)
        var runId = await CreateRunAsync(http, o, pipelineName);
        Console.WriteLine($"Pipeline runId: {runId}");

        Console.WriteLine("Done.");
        return 0;
    }

    // ---------------------------
    // REST helpers
    // ---------------------------

    private static async Task PutLinkedServiceAsync(HttpClient http, Options o, string linkedServiceName, object body)
    {
        // Linked Services - Create or Update endpoint (PUT) [2](https://learn.microsoft.com/en-us/rest/api/datafactory/linked-services/create-or-update?view=rest-datafactory-2018-06-01)
        var url = ArmUrl(o, $"linkedservices/{linkedServiceName}");
        await PutAsync(http, url, body);
        Console.WriteLine($"Upserted linked service: {linkedServiceName}");
    }

    private static async Task PutDatasetAsync(HttpClient http, Options o, string datasetName, object body)
    {
        // Datasets - Create or Update endpoint (PUT) [3](https://learn.microsoft.com/en-us/rest/api/datafactory/datasets/create-or-update?view=rest-datafactory-2018-06-01)
        var url = ArmUrl(o, $"datasets/{datasetName}");
        await PutAsync(http, url, body);
        Console.WriteLine($"Upserted dataset: {datasetName}");
    }

    private static async Task PutPipelineAsync(HttpClient http, Options o, string pipelineName, object body)
    {
        // Pipelines - Create or Update endpoint (PUT) [4](https://learn.microsoft.com/en-us/rest/api/datafactory/pipelines/create-or-update?view=rest-datafactory-2018-06-01)
        var url = ArmUrl(o, $"pipelines/{pipelineName}");
        await PutAsync(http, url, body);
        Console.WriteLine($"Upserted pipeline: {pipelineName}");
    }

    private static async Task<string> CreateRunAsync(HttpClient http, Options o, string pipelineName)
    {
        // Pipelines - Create Run endpoint (POST) [7](https://teams.microsoft.com/l/meeting/details?eventId=AAMkADM0NzczMzFkLTQ2NDYtNDdmYy04MmJmLTBkZmE4OTFhMjE3YgBGAAAAAABDIuXcFUzPEYOWAIBfOF2yBwDhkCp9pmKMSphiZMPL7T9XAAlfYwABAACI_cT-xQKbSIgBs7Ed6PQVAAe-LK_qAAA%3d)
        var url =
            $"https://management.azure.com/subscriptions/{o.SubscriptionId}/resourceGroups/{o.ResourceGroupName}/providers/Microsoft.DataFactory/factories/{o.FactoryName}/pipelines/{pipelineName}/createRun?api-version={ApiVersion}";

        using var req = new HttpRequestMessage(HttpMethod.Post, url);
        req.Content = new StringContent("{}", Encoding.UTF8, "application/json");

        using var resp = await http.SendAsync(req);
        var content = await resp.Content.ReadAsStringAsync();
        resp.EnsureSuccessStatusCode();

        using var doc = JsonDocument.Parse(content);
        return doc.RootElement.GetProperty("runId").GetString()!;
    }

    private static async Task PutAsync(HttpClient http, string url, object body)
    {
        var json = JsonSerializer.Serialize(body, new JsonSerializerOptions { WriteIndented = true });

        using var req = new HttpRequestMessage(HttpMethod.Put, url);
        // The PUT endpoints support If-Match for update; "*" forces unconditional update [2](https://learn.microsoft.com/en-us/rest/api/datafactory/linked-services/create-or-update?view=rest-datafactory-2018-06-01)[3](https://learn.microsoft.com/en-us/rest/api/datafactory/datasets/create-or-update?view=rest-datafactory-2018-06-01)[4](https://learn.microsoft.com/en-us/rest/api/datafactory/pipelines/create-or-update?view=rest-datafactory-2018-06-01)
        req.Headers.TryAddWithoutValidation("If-Match", "*");
        req.Content = new StringContent(json, Encoding.UTF8, "application/json");

        using var resp = await http.SendAsync(req);
        var content = await resp.Content.ReadAsStringAsync();
        if (!resp.IsSuccessStatusCode)
        {
            Console.Error.WriteLine(content);
        }
        resp.EnsureSuccessStatusCode();
    }

    private static string ArmUrl(Options o, string childPath)
        => $"https://management.azure.com/subscriptions/{o.SubscriptionId}/resourceGroups/{o.ResourceGroupName}/providers/Microsoft.DataFactory/factories/{o.FactoryName}/{childPath}?api-version={ApiVersion}";

    // ---------------------------
    // Payload builders
    // ---------------------------

    private static object BuildSqlServerLinkedService(string name, Options o, string integrationRuntimeName)
    {
        // This is a standard pattern for SQL Server LS; SHIR is referenced via connectVia (IntegrationRuntimeReference).
        // Integration runtime is the bridge between linked services and activities [1](https://learn.microsoft.com/en-us/azure/data-factory/concepts-integration-runtime)
        var connString =
            $"Server={o.SqlServerName};Database={o.SqlDatabaseName};User ID={o.SqlUser};Password={o.SqlPassword};TrustServerCertificate=True;";

        return new
        {
            name,
            properties = new
            {
                type = "SqlServer",
                typeProperties = new
                {
                    connectionString = new
                    {
                        type = "SecureString",
                        value = connString
                    }
                },
                connectVia = new
                {
                    referenceName = integrationRuntimeName,
                    type = "IntegrationRuntimeReference"
                }
            }
        };
    }

    private static object BuildFabricLakehouseLinkedService(string name, Options o)
    {
        // ADF Fabric Lakehouse LS: required properties per Microsoft Learn:
        // type=Lakehouse, workspaceId, artifactId, tenant, servicePrincipalId, servicePrincipalCredentialType, servicePrincipalCredential [9](https://learn.microsoft.com/en-us/azure/data-factory/connector-microsoft-fabric-lakehouse)
        return new
        {
            name,
            properties = new
            {
                type = "Lakehouse",
                typeProperties = new
                {
                    workspaceId = o.FabricWorkspaceId,
                    artifactId = o.FabricLakehouseId,
                    tenant = o.TenantId,
                    servicePrincipalId = o.ServicePrincipalId,
                    servicePrincipalCredentialType = "ServicePrincipalKey",
                    servicePrincipalCredential = new
                    {
                        type = "SecureString",
                        value = o.ServicePrincipalSecret
                    }
                }
                // connectVia is supported for the connector as a property [9](https://learn.microsoft.com/en-us/azure/data-factory/connector-microsoft-fabric-lakehouse)
            }
        };
    }

    private static object BuildSqlServerTableDataset(string name, string linkedServiceName, string tableName)
    {
        return new
        {
            name,
            properties = new
            {
                linkedServiceName = new { referenceName = linkedServiceName, type = "LinkedServiceReference" },
                type = "SqlServerTable",
                typeProperties = new
                {
                    tableName = tableName
                }
            }
        };
    }

    private static object BuildLakehouseTableDataset(string name, string linkedServiceName, string schema, string table)
    {
        // Schema must be populated as typeProperties.schema (NOT "schema.table" jammed into table) — this is exactly the customer ask [6](https://teams.microsoft.com/l/message/19:f7363443ad59447fa055130751739d4b@thread.v2/1773841494028?context=%7B%22contextType%22:%22chat%22%7D)
        // Microsoft Learn documents that typeProperties.schema is the schema name property for Lakehouse Table dataset [5](https://learn.microsoft.com/en-us/dotnet/api/microsoft.azure.management.datafactory.models.lakehousetabledataset.typepropertiesschema?view=az-ps-latest)
        return new
        {
            name,
            properties = new
            {
                linkedServiceName = new { referenceName = linkedServiceName, type = "LinkedServiceReference" },
                type = "LakehouseTable",
                typeProperties = new
                {
                    schema = schema,
                    table = table
                }
            }
        };
    }

    private static object BuildCopyPipeline(string pipelineName, string sourceDatasetName, string sinkDatasetName)
    {
        // Pipeline definition uses Copy activity; pipeline create/update is REST PUT [4](https://learn.microsoft.com/en-us/rest/api/datafactory/pipelines/create-or-update?view=rest-datafactory-2018-06-01)
        return new
        {
            name = pipelineName,
            properties = new
            {
                activities = new object[]
                {
                    new
                    {
                        name = "CopySqlToLakehouse",
                        type = "Copy",
                        inputs = new object[]
                        {
                            new { referenceName = sourceDatasetName, type = "DatasetReference" }
                        },
                        outputs = new object[]
                        {
                            new { referenceName = sinkDatasetName, type = "DatasetReference" }
                        },
                        typeProperties = new
                        {
                            source = new { type = "SqlServerSource" },
                            sink   = new { type = "LakehouseTableSink" }
                        }
                    }
                }
            }
        };
    }

    // ---------------------------
    // Options / CLI parsing
    // ---------------------------

    private sealed class Options
    {
        public string SubscriptionId { get; set; } = "";
        public string ResourceGroupName { get; set; } = "";
        public string FactoryName { get; set; } = "";
        public string IntegrationRuntimeName { get; set; } = "";

        public string SqlServerName { get; set; } = "";
        public string SqlDatabaseName { get; set; } = "";
        public string SqlUser { get; set; } = "";
        public string SqlPassword { get; set; } = "";
        public string SourceSqlTable { get; set; } = "";
        public string FabricWorkspaceId { get; set; } = "";
        public string FabricLakehouseId { get; set; } = "";
        public string TenantId { get; set; } = "";
        public string ServicePrincipalId { get; set; } = "";
        public string ServicePrincipalSecret { get; set; } = "";

        public string TargetSchema { get; set; } = "";
        public string TargetTable { get; set; } = "";

 
        public static Options? Parse(string[] args)
        {
            var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

            for (int i = 0; i < args.Length; i++)
            {
                if (args[i].StartsWith("--") && i + 1 < args.Length)
                {
                    dict[args[i][2..]] = args[i + 1];
                    i++;
                }
            }

            // Start with defaults (the property initializers)
            var o = new Options();

            // Only overwrite a default when a non-empty value is provided
            string GetOr(string key, string current)
                => (dict.TryGetValue(key, out var v) && !string.IsNullOrWhiteSpace(v)) ? v : current;

            o.SubscriptionId = GetOr("sub", o.SubscriptionId);
            o.ResourceGroupName = GetOr("rg", o.ResourceGroupName);
            o.FactoryName = GetOr("factory", o.FactoryName);
            o.IntegrationRuntimeName = GetOr("ir", o.IntegrationRuntimeName);

            o.SqlServerName = GetOr("sqlServer", o.SqlServerName);
            o.SqlDatabaseName = GetOr("sqlDb", o.SqlDatabaseName);
            o.SqlUser = GetOr("sqlUser", o.SqlUser);
            o.SqlPassword = GetOr("sqlPwd", o.SqlPassword);
            o.SourceSqlTable = GetOr("sqlTable", o.SourceSqlTable);

            o.FabricWorkspaceId = GetOr("workspaceId", o.FabricWorkspaceId);
            o.FabricLakehouseId = GetOr("lakehouseId", o.FabricLakehouseId);
            o.TenantId = GetOr("tenantId", o.TenantId);
            o.ServicePrincipalId = GetOr("spnId", o.ServicePrincipalId);
            o.ServicePrincipalSecret = GetOr("spnSecret", o.ServicePrincipalSecret);

            o.TargetSchema = GetOr("schema", o.TargetSchema);
            o.TargetTable = GetOr("table", o.TargetTable);

            Console.WriteLine(o);

            // minimal validation: now defaults count as valid
            var required = new[]
            {
        o.SubscriptionId, o.ResourceGroupName, o.FactoryName, o.IntegrationRuntimeName,
        o.SqlServerName, o.SqlDatabaseName, o.SqlUser, o.SqlPassword, o.SourceSqlTable,
        o.FabricWorkspaceId, o.FabricLakehouseId, o.TenantId, o.ServicePrincipalId, o.ServicePrincipalSecret
    };

            if (required.Any(string.IsNullOrWhiteSpace))
            {
                Console.Error.WriteLine("Missing required args. Example:");
                Console.Error.WriteLine("dotnet run -- " +
                    "--sub <subId> --rg <rg> --factory <adfName> --ir <shirName> " +
                    "--sqlServer <server> --sqlDb <db> --sqlUser <user> --sqlPwd <pwd> --sqlTable <dbo.Table> " +
                    "--workspaceId <fabricWorkspaceGuid> --lakehouseId <lakehouseArtifactGuid> --tenantId <tenantGuid> " +
                    "--spnId <appId> --spnSecret <secret> --schema <customSchema> --table <targetTable>");
                return null;
            }

            return o;
        }
    }
}
