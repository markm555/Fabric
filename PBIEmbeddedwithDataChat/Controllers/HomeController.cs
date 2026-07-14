using System.Threading.Tasks;
using Microsoft.AspNetCore.Mvc;
using AppOwnsData.Services;

namespace AppOwnsData.Controllers
{

    public class HomeController : Controller
    {

        private readonly PowerBiServiceApi powerBiServiceApi;

        public HomeController(PowerBiServiceApi powerBiServiceApi)
        {
            this.powerBiServiceApi = powerBiServiceApi;
        }

        public async Task<IActionResult> Index()
        {
            var viewModel = await this.powerBiServiceApi.GetReportEmbeddingData();
            return View(viewModel);
        }

        [HttpPost]
        public async Task<IActionResult> AskAgent([FromBody] AgentRequest request)
        {

            if (request == null || string.IsNullOrWhiteSpace(request.Question))
            {
                return BadRequest("Question required.");
            }

            string answer = await this.powerBiServiceApi.AskDataAgentAsync(request.Question);

            return Json(new
            {
                answer = answer,
                status = Response.StatusCode.ToString()
            });
        }

        public class AgentRequest
        {
            public string Question { get; set; }
        }
    }
}