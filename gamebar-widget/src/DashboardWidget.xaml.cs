using System;
using System.Globalization;
using System.Threading.Tasks;
using Windows.Security.Credentials;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Shapes;
using FoundationSunshineWidget.Services;

namespace FoundationSunshineWidget
{
    /// <summary>
    /// 串流仪表盘主视图。1s 轮询 /api/widget/state,四态:
    /// 串流中 / 无会话 / 数据过期(ts 停更 >5s 徽标) / 端点错误(持续重试)。
    /// 连接设置在无会话与错误态可见,保证首次配置与修正配置都有入口。
    /// token 存 PasswordVault(Credential Locker),LocalSettings 仅存端口与配置状态。
    /// 破坏性动作为两段式确认:第一次按下只上膛(3s 自动解除)。
    /// </summary>
    public sealed partial class DashboardWidget : Page
    {
        private const double StaleThresholdSec = 5.0;
        private const string VaultResource = "FoundationSunshineWidget";
        private const string VaultUserName = "sunshine-widget";
        private const string LegacyTokenSetting = "SunshineToken";

        private readonly DispatcherTimer _timer = new DispatcherTimer();
        private WidgetClient _client;
        private WidgetState _lastState;
        private bool _polling;
        private int _errorCount;

        private bool _stopArmed;
        private bool _restartArmed;
        private DispatcherTimer _stopReset;
        private DispatcherTimer _restartReset;

        public DashboardWidget()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(Windows.UI.Xaml.Navigation.NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            LoadSettings();
            _timer.Interval = TimeSpan.FromSeconds(1);
            _timer.Tick += PollTimer_Tick;
            _timer.Start();
        }

        protected override void OnNavigatedFrom(Windows.UI.Xaml.Navigation.NavigationEventArgs e)
        {
            _timer.Stop();
            base.OnNavigatedFrom(e);
        }

        private static string LoadTokenFromVault()
        {
            try
            {
                var vault = new PasswordVault();
                var credential = vault.Retrieve(VaultResource, VaultUserName);
                return credential.Password ?? string.Empty;
            }
            catch (Exception)
            {
                // 未存储时 Retrieve 抛异常
                return string.Empty;
            }
        }

        private static void SaveTokenToVault(string token)
        {
            var vault = new PasswordVault();
            try
            {
                vault.Remove(vault.Retrieve(VaultResource, VaultUserName));
            }
            catch (Exception)
            {
                // 无旧凭据时忽略
            }
            if (!string.IsNullOrEmpty(token))
            {
                vault.Add(new PasswordCredential(VaultResource, VaultUserName, token));
            }
        }

        private void LoadSettings()
        {
            var settings = ApplicationData.Current.LocalSettings;
            int port = 47990;
            object portVal;
            if (settings.Values.TryGetValue("SunshinePort", out portVal) && portVal is int)
            {
                port = (int)portVal;
            }

            // 一次性迁移:旧版本把 token 明文放在 LocalSettings,迁入 PasswordVault 后删除
            var legacyToken = settings.Values[LegacyTokenSetting] as string;
            if (!string.IsNullOrEmpty(legacyToken) && LoadTokenFromVault().Length == 0)
            {
                SaveTokenToVault(legacyToken);
            }
            settings.Values.Remove(LegacyTokenSetting);

            var token = LoadTokenFromVault();
            PortBox.Text = port.ToString(CultureInfo.InvariantCulture);
            // 令牌不回填到 PasswordBox,只显示配置状态
            TokenStatusText.Text = "令牌:" + (token.Length > 0 ? "已配置" : "未配置");
            RebuildClient(port, token);
        }

        private void RebuildClient(int port, string token)
        {
            _client = new WidgetClient(port, token);
            _errorCount = 0;
        }

        private void SaveSettings_Click(object sender, RoutedEventArgs e)
        {
            int port;
            if (!int.TryParse(PortBox.Text.Trim(), out port) || port < 1 || port > 65535)
            {
                SettingsHint.Text = "端口无效";
                return;
            }

            var settings = ApplicationData.Current.LocalSettings;
            settings.Values["SunshinePort"] = port;

            // PasswordBox 为空 = 保留已存令牌;非空 = 替换
            var token = LoadTokenFromVault();
            var entered = TokenBox.Password.Trim();
            if (entered.Length > 0)
            {
                SaveTokenToVault(entered);
                token = entered;
                TokenBox.Password = string.Empty;
            }
            TokenStatusText.Text = "令牌:" + (token.Length > 0 ? "已配置" : "未配置");

            RebuildClient(port, token);
            SettingsHint.Text = "已保存";
        }

        private void ClearToken_Click(object sender, RoutedEventArgs e)
        {
            SaveTokenToVault(string.Empty);
            TokenBox.Password = string.Empty;
            TokenStatusText.Text = "令牌:未配置";
            SettingsHint.Text = "令牌已清除";
        }

        private async void PollTimer_Tick(object sender, object e)
        {
            if (_polling) return;
            _polling = true;
            try
            {
                var state = await _client.GetStateAsync();
                _lastState = state;
                _errorCount = 0;
                Render(state);
            }
            catch (Exception ex)
            {
                _errorCount++;
                ShowError(ex.Message);
            }
            finally
            {
                _polling = false;
            }
        }

        private void ShowError(string message)
        {
            MainPanel.Visibility = Visibility.Collapsed;
            EmptyPanel.Visibility = Visibility.Collapsed;
            ErrorPanel.Visibility = Visibility.Visible;
            // 设置区保持可见:首次配置与修正错误配置都必须有入口
            SettingsPanel.Visibility = Visibility.Visible;
            StaleBadge.Visibility = Visibility.Collapsed;
            ErrorDetail.Text = "GET /api/widget/state 失败:" + message;
            RetryText.Text = _errorCount > 1
                ? "每秒自动重试 · 已失败 " + _errorCount + " 次"
                : "每秒自动重试";
        }

        private void Render(WidgetState s)
        {
            ErrorPanel.Visibility = Visibility.Collapsed;
            StaleBadge.Visibility = Visibility.Collapsed;

            if (!s.HasSession)
            {
                MainPanel.Visibility = Visibility.Collapsed;
                EmptyPanel.Visibility = Visibility.Visible;
                SettingsPanel.Visibility = Visibility.Visible;
                return;
            }

            EmptyPanel.Visibility = Visibility.Collapsed;
            SettingsPanel.Visibility = Visibility.Collapsed;
            MainPanel.Visibility = Visibility.Visible;

            // 数据过期:保留画面,只亮徽标
            if (s.IsStale(DateTimeOffset.Now, StaleThresholdSec))
            {
                StaleText.Text = "数据过期 " + Math.Floor(s.StaleSeconds(DateTimeOffset.Now)) + " s";
                StaleBadge.Visibility = Visibility.Visible;
            }

            ClientText.Text = s.ClientName ?? "未知客户端";
            AppText.Text = string.IsNullOrEmpty(s.AppName) ? "Desktop" : s.AppName;
            MetaText.Text = string.Format(CultureInfo.InvariantCulture,
                "{0}×{1} · {2} fps · {3} Mbps{4}",
                s.Width, s.Height, s.Fps, (s.BitrateKbps + 999) / 1000, s.Hdr ? " · HDR" : "");
            EncoderText.Text = string.IsNullOrEmpty(s.Encoder) ? "—" : s.Encoder;
            UptimeText.Text = FormatUptime(s.UptimeSec);

            RenderFps(s);
            RenderBars(s);
        }

        private void RenderFps(WidgetState s)
        {
            double fps = s.RecentFps.Count > 0 ? s.RecentFps[s.RecentFps.Count - 1] : 0;
            FpsText.Text = fps > 0 ? fps.ToString("F1", CultureInfo.InvariantCulture) : "—";

            double mean = 0;
            foreach (var v in s.RecentFps) mean += v;
            if (s.RecentFps.Count > 0) mean /= s.RecentFps.Count;
            if (mean > 0)
            {
                double delta = (fps - mean) / mean * 100;
                FpsDeltaText.Text = (delta >= 0 ? "+" : "−") +
                    Math.Abs(delta).ToString("F1", CultureInfo.InvariantCulture) + "%";
            }
            else
            {
                FpsDeltaText.Text = "";
            }

            double target = s.Fps > 0 ? s.Fps : 120;
            TargetNoteText.Text = "虚线 = 目标 " + target.ToString("0", CultureInfo.InvariantCulture) + " fps";

            RedrawSparkline(target);
        }

        private void SparkCanvas_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (_lastState != null)
            {
                RedrawSparkline(_lastState.Fps > 0 ? _lastState.Fps : 120);
            }
        }

        private void RedrawSparkline(double target)
        {
            var fps = _lastState != null ? _lastState.RecentFps : null;
            double w = SparkCanvas.ActualWidth;
            double h = SparkCanvas.ActualHeight;
            if (w <= 0 || h <= 0 || fps == null || fps.Count == 0) return;

            double lo = target * 0.75;
            double hi = target * 1.1;

            TargetLine.X1 = 0;
            TargetLine.X2 = w;
            TargetLine.Y1 = MapY(h, lo, hi, target);
            TargetLine.Y2 = TargetLine.Y1;

            if (fps.Count == 1)
            {
                // 单样本:只画末端点,不画线
                FpsLine.Points = new PointCollection();
                double y = MapY(h, lo, hi, fps[0]);
                Canvas.SetLeft(FpsEndDot, w - 3.5);
                Canvas.SetTop(FpsEndDot, y - 3.5);
                return;
            }

            var points = new PointCollection();
            for (int i = 0; i < fps.Count; i++)
            {
                double x = w * i / (fps.Count - 1);
                double y = MapY(h, lo, hi, fps[i]);
                points.Add(new Windows.Foundation.Point(x, y));
            }
            FpsLine.Points = points;

            var last = points[points.Count - 1];
            Canvas.SetLeft(FpsEndDot, last.X - 3.5);
            Canvas.SetTop(FpsEndDot, last.Y - 3.5);
        }

        private static double MapY(double height, double lo, double hi, double value)
        {
            value = Math.Max(lo, Math.Min(hi, value));
            return height - 4 - (value - lo) / (hi - lo) * (height - 8);
        }

        private void RenderBars(WidgetState s)
        {
            double target = s.Fps > 0 ? s.Fps : 120;
            double budget = 1000.0 / target;
            BudgetLegend.Text = "│ 帧预算 " + budget.ToString("F1", CultureInfo.InvariantCulture) +
                " ms(按 " + target.ToString("0", CultureInfo.InvariantCulture) + " fps 目标计算)";

            SetBar(Bar1, Mark1, Val1, Track1, s.CaptureToConvertP95, budget, false);
            SetBar(Bar2, Mark2, Val2, Track2, s.EncodeP95, budget, false);
            SetBar(Bar3, Mark3, Val3, Track3, s.PacketToBroadcastP95, budget, false);
            SetBar(Bar4, Mark4, Val4, Track4, s.TotalP95, budget, true);

            string send = s.PacketToBroadcastP95.HasValue
                ? s.PacketToBroadcastP95.Value.ToString("F1", CultureInfo.InvariantCulture) + " ms"
                : "—";
            NetText.Text = "下行发送 " + send + " · 客户端报告丢包见客户端 overlay";
        }

        private static void SetBar(Rectangle bar, Rectangle mark, TextBlock val, Grid track,
            double? p95, double budget, bool judgeOver)
        {
            if (!p95.HasValue || track.ActualWidth <= 0)
            {
                bar.Width = 0;
                val.Text = "—";
                return;
            }
            double maxScale = budget * 2;
            double ratio = Math.Min(1.0, p95.Value / maxScale);
            bar.Width = ratio * track.ActualWidth;
            Canvas.SetLeft(mark, 0.5 * track.ActualWidth); // 预算 = 比例尺中点

            bool over = judgeOver && p95.Value > budget;
            bar.Fill = over
                ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xFF, 0xFF, 0xC8, 0x3D))
                : new SolidColorBrush(Windows.UI.Color.FromArgb(0xFF, 0x60, 0xCD, 0xFF));
            val.Text = p95.Value.ToString("F1", CultureInfo.InvariantCulture) + " ms" +
                (over ? "  ▲ 超预算" : "");
        }

        private void Track_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (_lastState != null) RenderBars(_lastState);
        }

        private static string FormatUptime(double sec)
        {
            int total = (int)Math.Floor(sec);
            int h = total / 3600;
            int m = (total % 3600) / 60;
            int s = total % 60;
            return h > 0
                ? string.Format(CultureInfo.InvariantCulture, "{0}:{1:D2}:{2:D2}", h, m, s)
                : string.Format(CultureInfo.InvariantCulture, "{0:D2}:{1:D2}", m, s);
        }

        // ===== 动作:两段式确认 =====

        private async void StopButton_Click(object sender, RoutedEventArgs e)
        {
            if (!_stopArmed)
            {
                Arm(ref _stopArmed, ref _stopReset, StopButton, "确认断开所有会话?", StopDisarm);
                return;
            }
            Disarm(ref _stopArmed, ref _stopReset, StopButton, "断开所有会话");
            try
            {
                await _client.PostActionAsync("stop_all_sessions");
                ShowStatus("已请求断开全部串流会话");
            }
            catch (Exception ex)
            {
                ShowStatus("断开失败:" + ex.Message);
            }
        }

        private void StopDisarm()
        {
            Disarm(ref _stopArmed, ref _stopReset, StopButton, "断开所有会话");
        }

        private async void RestartButton_Click(object sender, RoutedEventArgs e)
        {
            if (!_restartArmed)
            {
                Arm(ref _restartArmed, ref _restartReset, RestartButton, "确认重启?", RestartDisarm);
                return;
            }
            Disarm(ref _restartArmed, ref _restartReset, RestartButton, "重启 Foundation Sunshine 服务");
            try
            {
                await _client.PostActionAsync("restart_service");
                ShowStatus("已请求重启,数据将在服务恢复后自动续上");
            }
            catch (Exception ex)
            {
                ShowStatus("重启请求失败:" + ex.Message);
            }
        }

        private void RestartDisarm()
        {
            Disarm(ref _restartArmed, ref _restartReset, RestartButton, "重启 Foundation Sunshine 服务");
        }

        private static void Arm(ref bool armed, ref DispatcherTimer reset, Button button,
            string confirmText, Action onTimeout)
        {
            armed = true;
            button.Content = confirmText;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
            timer.Tick += (s, e) =>
            {
                timer.Stop();
                onTimeout();
            };
            timer.Start();
            reset = timer;
        }

        private static void Disarm(ref bool armed, ref DispatcherTimer reset, Button button, string normalText)
        {
            armed = false;
            if (reset != null)
            {
                reset.Stop();
                reset = null;
            }
            button.Content = normalText;
        }

        private async void ShowStatus(string message)
        {
            StatusText.Text = message;
            await System.Threading.Tasks.Task.Delay(3000);
            StatusText.Text = "";
        }
    }
}
