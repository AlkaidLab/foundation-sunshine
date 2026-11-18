using System.IO.Pipes;
using System.Net;
using System.Net.Http.Headers;

namespace Sparxie.Launch;

internal sealed class GrpcStatusException(int status, string message) : Exception($"Broker answered gRPC status {status}: {message}")
{
    public int Status { get; } = status;
}

internal sealed record StartResponse(bool Accepted, string SessionId, int Stage, int ErrorCode, string Message)
{
    public static StartResponse Parse(byte[] bytes)
    {
        var f = Proto.Parse(bytes);
        return new StartResponse(Proto.Bool(f, 1), Proto.String(f, 2), Proto.Int(f, 3), Proto.Int(f, 4), Proto.String(f, 5));
    }
}

internal sealed record SessionEvent(string SessionId, string State, int Stage, int ErrorCode, string Message)
{
    public static SessionEvent Parse(byte[] bytes)
    {
        var f = Proto.Parse(bytes);
        return new SessionEvent(Proto.String(f, 1), Proto.String(f, 2), Proto.Int(f, 3), Proto.Int(f, 4), Proto.String(f, 5));
    }
}

/// <summary>The Ping answer: protocol, Broker version and the optional features the Broker lists (field 3, repeated).</summary>
internal sealed record PingInfo(int Protocol, string BrokerVersion, IReadOnlyList<string> Capabilities)
{
    /// <summary>HoyoSettings.touch_ui_enabled (field 11): a session without the touch interface.</summary>
    public const string TouchUiSwitch = "touch-ui-switch";

    /// <summary>A Broker that understands game = "endfield" sessions (and the Endfield fields 12-16 of the settings).</summary>
    public const string Endfield = "endfield";

    /// <summary>ProfileSnapshot.screen (field 6): the session's size and window mode as Unity arguments of the game.</summary>
    public const string ScreenArgsCapability = "screen-args";

    public bool Has(string capability) => Capabilities.Contains(capability);

    public static PingInfo Parse(byte[] bytes)
    {
        var f = Proto.Parse(bytes);
        var caps = f.Where(x => x.Number == 3 && x.Wire == 2).Select(x => System.Text.Encoding.UTF8.GetString(x.Bytes)).ToList();
        return new PingInfo(Proto.Int(f, 1), Proto.String(f, 2), caps);
    }

    public override string ToString() =>
        $"protocol {Protocol} broker {BrokerVersion}" + (Capabilities.Count > 0 ? " capabilities " + string.Join(",", Capabilities) : " capabilities none");
}

/// <summary>HTTP/2 gRPC over the Broker's named pipe. Every call has its own time limit.</summary>
internal sealed class BrokerRpc : IDisposable
{
    public const int ProtocolVersion = 1;
    private const string Service = "sparxie.rpc.SparxieBroker";
    private readonly HttpClient _http;

    public BrokerRpc(string pipeName)
    {
        var handler = new SocketsHttpHandler
        {
            ConnectTimeout = TimeSpan.FromSeconds(2),
            ConnectCallback = async (_, cancellation) =>
            {
                var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                try
                {
                    await pipe.ConnectAsync(1500, cancellation).ConfigureAwait(false);
                    return pipe;
                }
                catch
                {
                    pipe.Dispose();
                    throw;
                }
            },
        };
        _http = new HttpClient(handler)
        {
            Timeout = Timeout.InfiniteTimeSpan, // each call has its own token
            DefaultRequestVersion = HttpVersion.Version20,
            DefaultVersionPolicy = HttpVersionPolicy.RequestVersionExact,
        };
    }

    public async Task<PingInfo> PingAsync(TimeSpan limit)
    {
        using var cancel = new CancellationTokenSource(limit);
        var payload = Proto.Message((1, Proto.Int(ProtocolVersion)), (2, Proto.String(Guid.NewGuid().ToString("N"))));
        var reply = await UnaryAsync("Ping", payload, cancel.Token).ConfigureAwait(false);
        return PingInfo.Parse(reply);
    }

    /// <summary>The StartSession request: protocol version, request id, and the profile with every Sparxie setting.</summary>
    internal static byte[] StartSessionPayload(Profile profile, string requestId)
    {
        var nested = Proto.Message((1, Proto.String(profile.Id)), (2, Proto.String(profile.DisplayName)), (3, Proto.String(profile.Game)), (4, Proto.String(profile.ExecutablePath)));
        if (profile.Hoyo is { } h)
        {
            var fields = new List<(int Field, Proto.Value Value)>
            {
                (1, Proto.Bool(h.FpsUnlock)), (2, Proto.Int(h.TargetFps)), (3, Proto.Bool(h.BackgroundLimit)), (4, Proto.Int(h.BackgroundFps)),
                (5, Proto.String(h.Priority)), (6, Proto.Bool(h.FollowPreset)), (7, Proto.Int(h.Preset30)), (8, Proto.Int(h.Preset60)),
                (9, Proto.Bool(h.ScaleOverride)), (10, Proto.Int(h.Scale)),
                // optional bool touch_ui_enabled = 11: always sent, so a Broker that knows it never has to guess; one that does not skips it (and Ping tells)
                (11, Proto.Bool(h.TouchUi)),
            };
            // an Endfield session also carries its input, resolution and refresh rate (12-16); the other games leave them out
            if (profile.Game == "endfield")
            {
                fields.Add((12, Proto.String(h.EndfieldInput)));
                fields.Add((13, Proto.Bool(h.EndfieldAutoResolution)));
                fields.Add((14, Proto.Int(h.EndfieldWidth)));
                fields.Add((15, Proto.Int(h.EndfieldHeight)));
                fields.Add((16, Proto.Int(h.EndfieldRefresh)));
            }
            var settings = Proto.Message(fields.ToArray());
            nested = Proto.Append(nested, 5, settings);
        }
        // ScreenArgs screen = 6 { width 1, height 2, window_mode 3 }; only when there is something to say (a Broker without the capability must not be sent it)
        if (profile.Screen is { IsEmpty: false } screen)
            nested = Proto.Append(nested, 6, Proto.Message((1, Proto.Int(screen.Width)), (2, Proto.Int(screen.Height)), (3, Proto.String(screen.WindowMode))));
        return Proto.Message((1, Proto.Int(ProtocolVersion)), (2, Proto.String(requestId)), (3, Proto.Bytes(nested)));
    }

    public async Task<StartResponse> StartSessionAsync(Profile profile, TimeSpan limit)
    {
        var payload = StartSessionPayload(profile, Guid.NewGuid().ToString("N"));
        using var cancel = new CancellationTokenSource(limit);
        return StartResponse.Parse(await UnaryAsync("StartSession", payload, cancel.Token).ConfigureAwait(false));
    }

    /// <summary>
    /// Opens the single event stream. The Broker sends no response headers until its first event, so the request is only started here
    /// and the returned task finishes when the stream is established. Events queue inside the Broker, so none are lost to the wait.
    /// </summary>
    public EventStream OpenEvents(CancellationToken cancellation)
    {
        var payload = Proto.Message((1, Proto.Int(ProtocolVersion)), (2, Proto.String(Guid.NewGuid().ToString("N"))));
        return new EventStream(SendAsync("StreamEvents", payload, cancellation));
    }

    private async Task<byte[]> UnaryAsync(string method, byte[] payload, CancellationToken cancellation)
    {
        using var response = await SendAsync(method, payload, cancellation).ConfigureAwait(false);
        await using var stream = await response.Content.ReadAsStreamAsync(cancellation).ConfigureAwait(false);
        var frame = await Grpc.ReadFrameAsync(stream, cancellation).ConfigureAwait(false);
        CheckStatus(response);
        return frame ?? throw new InvalidDataException("the Broker returned no message");
    }

    private async Task<HttpResponseMessage> SendAsync(string method, byte[] payload, CancellationToken cancellation)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, $"http://localhost/{Service}/{method}")
        {
            Version = HttpVersion.Version20,
            VersionPolicy = HttpVersionPolicy.RequestVersionExact,
            Content = new ByteArrayContent(Grpc.Frame(payload)),
        };
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/grpc");
        request.Headers.TryAddWithoutValidation("TE", "trailers");
        var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation).ConfigureAwait(false);
        if (response.StatusCode != HttpStatusCode.OK)
        {
            var code = (int)response.StatusCode;
            response.Dispose();
            throw new HttpRequestException($"Broker HTTP status {code}");
        }
        return response;
    }

    /// <summary>gRPC puts its status in the trailers, or in the headers of an answer that has no body.</summary>
    public static void CheckStatus(HttpResponseMessage response)
    {
        if (!TryGet(response, "grpc-status", out var raw)) return;
        if (!int.TryParse(raw, out var status) || status == 0) return;
        TryGet(response, "grpc-message", out var message);
        throw new GrpcStatusException(status, Uri.UnescapeDataString(message ?? ""));
    }

    private static bool TryGet(HttpResponseMessage response, string name, out string? value)
    {
        value = null;
        if (response.TrailingHeaders.TryGetValues(name, out var t)) { value = t.FirstOrDefault(); return true; }
        if (response.Headers.TryGetValues(name, out var h)) { value = h.FirstOrDefault(); return true; }
        return false;
    }

    public void Dispose() => _http.Dispose();
}

internal sealed class EventStream : IDisposable
{
    private readonly Task<HttpResponseMessage> _opening;
    private HttpResponseMessage? _response;

    public EventStream(Task<HttpResponseMessage> opening) { _opening = opening; }

    /// <summary>The next event; null when the Broker closed the stream.</summary>
    public async IAsyncEnumerable<SessionEvent> ReadAsync([System.Runtime.CompilerServices.EnumeratorCancellation] CancellationToken cancellation)
    {
        _response = await _opening.ConfigureAwait(false);
        await using var stream = await _response.Content.ReadAsStreamAsync(cancellation).ConfigureAwait(false);
        while (true)
        {
            var frame = await Grpc.ReadFrameAsync(stream, cancellation).ConfigureAwait(false);
            if (frame is null) break;
            yield return SessionEvent.Parse(frame);
        }
        BrokerRpc.CheckStatus(_response);
    }

    public void Dispose()
    {
        if (_opening.IsCompletedSuccessfully) _opening.Result.Dispose();
        _response?.Dispose();
    }
}
