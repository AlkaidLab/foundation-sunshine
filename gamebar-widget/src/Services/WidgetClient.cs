using System;
using System.Threading.Tasks;
using Windows.Data.Json;
using Windows.Security.Cryptography.Certificates;
using Windows.Storage.Streams;
using Windows.Web.Http;
using Windows.Web.Http.Filters;

namespace FoundationSunshineWidget.Services
{
    /// <summary>
    /// Foundation Sunshine 本地端点客户端。
    /// GET  /api/widget/state   (1s 轮询)
    /// POST /api/widget/action  {"type":"stop_session"|"restart_service"}
    /// 认证:X-Sunshine-Token,与 sunshine.conf 的 widget_token 一致。
    /// 自签证书:忽略 Untrusted/InvalidCa 链验证错误(仅本地回环)。
    /// 注意:packaged app 访问回环需先加 LoopbackExempt 豁免,见 README。
    /// </summary>
    public sealed class WidgetClient
    {
        private readonly HttpClient _http;
        private readonly string _token;
        private readonly Uri _stateUri;
        private readonly Uri _actionUri;

        public WidgetClient(int port, string token)
        {
            _token = token ?? string.Empty;

            var filter = new HttpBaseProtocolFilter();
            // Sunshine 的 HTTPS 是自签证书
            filter.IgnorableServerCertificateErrors.Add(ChainValidationResult.Untrusted);
            _http = new HttpClient(filter);
            _http.DefaultRequestHeaders.Append("X-Sunshine-Token", _token);

            var baseUri = new Uri("https://localhost:" + port + "/");
            _stateUri = new Uri(baseUri, "/api/widget/state");
            _actionUri = new Uri(baseUri, "/api/widget/action");
        }

        public async Task<WidgetState> GetStateAsync()
        {
            using (var response = await _http.GetAsync(_stateUri))
            {
                if (response.StatusCode == HttpStatusCode.NotFound)
                {
                    throw new InvalidOperationException("服务器未启用 widget_token(sunshine.conf 中配置后重启)");
                }
                if (response.StatusCode == HttpStatusCode.Unauthorized)
                {
                    throw new InvalidOperationException("令牌无效,请核对小部件设置中的 token");
                }
                response.EnsureSuccessStatusCode();
                string body = await response.Content.ReadAsStringAsync();
                JsonObject root;
                if (!JsonObject.TryParse(body, out root))
                {
                    throw new InvalidOperationException("响应不是合法 JSON");
                }
                return WidgetState.FromJson(root);
            }
        }

        public async Task PostActionAsync(string type)
        {
            var payload = new JsonObject();
            payload.Add("type", JsonValue.CreateStringValue(type));
            var content = new HttpStringContent(payload.Stringify(),
                UnicodeEncoding.Utf8, "application/json");
            using (var response = await _http.PostAsync(_actionUri, content))
            {
                if (response.StatusCode == HttpStatusCode.Unauthorized)
                {
                    throw new InvalidOperationException("令牌无效,请核对小部件设置中的 token");
                }
                response.EnsureSuccessStatusCode();
            }
        }
    }
}
