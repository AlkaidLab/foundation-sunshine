using System;
using System.Collections.Generic;
using Windows.Data.Json;

namespace FoundationSunshineWidget.Services
{
    /// <summary>
    /// /api/widget/state 的客户端模型。字段对齐服务端 perf 快照
    /// (src/perf_recorder.cpp 的 make_session_json)+ 会话信息补充字段。
    /// 所有数值字段容错:缺失为 null。
    /// </summary>
    public sealed class WidgetState
    {
        public long TsMs { get; private set; }
        public bool HasSession { get; private set; }
        public string ClientName { get; private set; }
        public string AppName { get; private set; }
        public string State { get; private set; }
        public int Width { get; private set; }
        public int Height { get; private set; }
        public int Fps { get; private set; }
        public int BitrateKbps { get; private set; }
        public bool Hdr { get; private set; }
        public string Encoder { get; private set; }
        public double UptimeSec { get; private set; }

        public List<double> RecentFps { get; private set; }
        public double? CaptureToConvertP95 { get; private set; }
        public double? EncodeQueueP95 { get; private set; }
        public double? EncodeP95 { get; private set; }
        public double? PacketToBroadcastP95 { get; private set; }
        public double? TotalP95 { get; private set; }
        public double? TotalLast { get; private set; }

        public bool IsStale(DateTimeOffset now, double thresholdSec)
        {
            // ts 是服务器 Unix 毫秒;与时钟偏差相比,更稳妥的是本地收到时间。
            // 这里仍用 ts 判定,因为"服务器停更"与"网络断"都应表现为过期。
            var ageMs = now.ToUnixTimeMilliseconds() - TsMs;
            return ageMs > thresholdSec * 1000;
        }

        public double StaleSeconds(DateTimeOffset now)
        {
            return Math.Max(0, (now.ToUnixTimeMilliseconds() - TsMs) / 1000.0);
        }

        private static double? GetP95(JsonObject stage)
        {
            if (stage == null) return null;
            IJsonValue v;
            if (stage.TryGetValue("p95_ms", out v) && v != null && v.ValueType == JsonValueType.Number)
            {
                return v.GetNumber();
            }
            return null;
        }

        private static List<double> ReadFps(JsonObject latency)
        {
            var result = new List<double>();
            if (latency == null) return result;
            IJsonValue v;
            // 服务端当前输出单个数值(由采样窗口推算的最新帧率);兼容未来的数组历史序列
            if (latency.TryGetValue("recent_fps", out v) && v != null)
            {
                if (v.ValueType == JsonValueType.Number)
                {
                    result.Add(v.GetNumber());
                }
                else if (v.ValueType == JsonValueType.Array)
                {
                    foreach (var item in v.GetArray())
                    {
                        if (item.ValueType == JsonValueType.Number)
                        {
                            result.Add(item.GetNumber());
                        }
                    }
                }
            }
            return result;
        }

        public static WidgetState FromJson(JsonObject root)
        {
            var s = new WidgetState();
            IJsonValue v;

            if (root.TryGetValue("ts", out v) && v.ValueType == JsonValueType.Number)
            {
                s.TsMs = (long)v.GetNumber();
            }

            if (root.TryGetValue("sessions", out v) && v.ValueType == JsonValueType.Array)
            {
                foreach (var item in v.GetArray())
                {
                    if (item.ValueType != JsonValueType.Object) continue;
                    JsonObject session = item.GetObject();
                    IJsonValue active;
                    bool isActive = session.TryGetValue("active", out active) &&
                                    active.ValueType == JsonValueType.Boolean &&
                                    active.GetBoolean();
                    if (!isActive) continue;

                    s.HasSession = true;
                    if (session.TryGetValue("client_name", out v)) s.ClientName = v.GetString();
                    if (session.TryGetValue("app", out v) && v.ValueType == JsonValueType.String) s.AppName = v.GetString();
                    if (session.TryGetValue("state", out v) && v.ValueType == JsonValueType.String) s.State = v.GetString();
                    if (session.TryGetValue("width", out v) && v.ValueType == JsonValueType.Number) s.Width = (int)v.GetNumber();
                    if (session.TryGetValue("height", out v) && v.ValueType == JsonValueType.Number) s.Height = (int)v.GetNumber();
                    if (session.TryGetValue("fps", out v) && v.ValueType == JsonValueType.Number) s.Fps = (int)v.GetNumber();
                    if (session.TryGetValue("bitrate_kbps", out v) && v.ValueType == JsonValueType.Number) s.BitrateKbps = (int)v.GetNumber();
                    if (session.TryGetValue("hdr", out v) && v.ValueType == JsonValueType.Boolean) s.Hdr = v.GetBoolean();
                    if (session.TryGetValue("encoder", out v) && v.ValueType == JsonValueType.String) s.Encoder = v.GetString();
                    if (session.TryGetValue("uptime_ms", out v) && v.ValueType == JsonValueType.Number) s.UptimeSec = v.GetNumber() / 1000.0;

                    IJsonValue pipeline;
                    if (session.TryGetValue("pipeline", out pipeline) && pipeline.ValueType == JsonValueType.Object)
                    {
                        JsonObject p = pipeline.GetObject();
                        IJsonValue stage;
                        if (p.TryGetValue("capture_to_convert", out stage) && stage.ValueType == JsonValueType.Object) s.CaptureToConvertP95 = GetP95(stage.GetObject());
                        if (p.TryGetValue("encode_queue", out stage) && stage.ValueType == JsonValueType.Object) s.EncodeQueueP95 = GetP95(stage.GetObject());
                        if (p.TryGetValue("encode", out stage) && stage.ValueType == JsonValueType.Object) s.EncodeP95 = GetP95(stage.GetObject());
                        if (p.TryGetValue("packet_to_broadcast", out stage) && stage.ValueType == JsonValueType.Object) s.PacketToBroadcastP95 = GetP95(stage.GetObject());
                        if (p.TryGetValue("total", out stage) && stage.ValueType == JsonValueType.Object) s.TotalP95 = GetP95(stage.GetObject());
                        if (p.TryGetValue("total", out stage) && stage.ValueType == JsonValueType.Object)
                        {
                            JsonObject total = stage.GetObject();
                            IJsonValue last;
                            if (total.TryGetValue("last_ms", out last) && last.ValueType == JsonValueType.Number) s.TotalLast = last.GetNumber();
                        }
                    }

                    IJsonValue hostLatency;
                    if (session.TryGetValue("host_latency", out hostLatency) && hostLatency.ValueType == JsonValueType.Object)
                    {
                        s.RecentFps = ReadFps(hostLatency.GetObject());
                    }
                    if (s.RecentFps == null) s.RecentFps = new List<double>();
                    break; // 只取首个活跃会话
                }
            }

            if (s.RecentFps == null) s.RecentFps = new List<double>();
            return s;
        }
    }
}
