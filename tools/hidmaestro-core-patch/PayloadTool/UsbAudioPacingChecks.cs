using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;

/// <summary>Device-free regression checks against the pinned SDK's real audio engine.</summary>
internal static class UsbAudioPacingChecks
{
    /// <summary>Checks audio data, deadline ordering, cancellation and pump shutdown without attaching devices.</summary>
    internal static void Run(Assembly assembly, string profilePath)
    {
        // Instantiate only the managed audio engine. No device, driver or
        // registry setup runs, so these checks also run in Windows CI.
        var profileType = assembly.GetType("HIDMaestro.Internal.ControllerProfile", true)!;
        var json = JsonNode.Parse(File.ReadAllText(profilePath))!.AsObject();
        json.Remove("layout");
        var profile = JsonSerializer.Deserialize(json.ToJsonString(), profileType)!;
        using (var engine = new Engine(assembly, profile))
        {
            var pcm = Enumerable.Range(0, 8 * 384).Select(i => (byte)(i * 17)).ToArray();
            engine.Submit(1, isIn: false, packets: 8, pcm);
            var output = engine.Next();
            Require(output.Sequence == 1 && output.Data is null, "OUT completion changed");
            Require(engine.Output.TryDequeue(out var actual) && actual.SequenceEqual(pcm),
                "OUT PCM was changed, dropped or duplicated");

            engine.Submit(2, isIn: true, packets: 8);
            var input = engine.Next();
            Require(input.Sequence == 2 && input.PerPacket == 192 && input.Data?.Length == 8 * 192,
                "IN packet size changed");
            Require(input.Data!.All(b => b == 0), "microphone underrun did not fill silence");

            // A new direction has its own cursor and must wake a pump that
            // is waiting for a much later OUT deadline. The generous timeout
            // checks progress, not microsecond scheduling precision in CI.
            engine.Submit(3, isIn: false, packets: 10_000);
            Thread.Sleep(20);
            engine.Submit(4, isIn: true, packets: 1);
            Require(engine.Next().Sequence == 4, "earlier IN work waited behind the OUT deadline");
            Require(engine.Unlink(3) && !engine.Unlink(3), "pending URB unlink changed");

            engine.Submit(5, isIn: true, packets: 10_000);
            // Disposing with an armed timer must wake and stop its thread.
        }
        using (var cleared = new Engine(assembly, profile))
        {
            cleared.Submit(6, isIn: false, packets: 1000);
            cleared.Clear();
            Thread.Sleep(1100);
            cleared.Submit(7, isIn: true, packets: 1);
            Require(cleared.Next().Sequence == 7, "cleared URB completed or blocked later work");
            // Dispose after completion, while the pump is idle.
        }
        Console.WriteLine("PASS: audio deadlines, OUT PCM, IN silence, earlier-work wakeup, unlink/clear and idle/armed shutdown");
    }

    /// <summary>Fails a regression check with a diagnostic carried by the build log.</summary>
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    /// <summary>A completion captured on the SDK's pacing thread.</summary>
    private sealed record Completion(uint Sequence, byte[]? Data, int PerPacket);

    /// <summary>Adapts internal SDK methods for behavioral tests without creating a controller or transport.</summary>
    private sealed class Engine : IDisposable
    {
        private const BindingFlags Fields = BindingFlags.Instance | BindingFlags.Public;
        private readonly IDisposable _instance;
        private readonly Type _pending;
        private readonly MethodInfo _submit;
        private readonly MethodInfo _unlink;
        private readonly MethodInfo _clear;
        private readonly FieldInfo _sequence;
        private readonly FieldInfo _due;
        private readonly Thread _thread;
        private readonly BlockingCollection<Completion> _completed = new();
        internal readonly ConcurrentQueue<byte[]> Output = new();

        /// <summary>Creates the managed audio pump and observes its completion and OUT PCM callbacks.</summary>
        internal Engine(Assembly assembly, object profile)
        {
            var type = assembly.GetType("HIDMaestro.Internal.Usbip.UsbAudioEngine", true)!;
            _pending = type.GetNestedType("PendingIso", BindingFlags.Public | BindingFlags.NonPublic)!;
            _sequence = _pending.GetField("Seqnum", Fields)!;
            _due = _pending.GetField("DueTick", Fields)!;
            var ctor = type.GetConstructors().Single();
            var callback = typeof(Engine).GetMethod(nameof(Completed), BindingFlags.Instance | BindingFlags.NonPublic)!
                .MakeGenericMethod(_pending).CreateDelegate(ctor.GetParameters()[1].ParameterType, this);
            _instance = (IDisposable)ctor.Invoke(new[] { profile, callback });
            _submit = type.GetMethod("SubmitIso")!;
            _unlink = type.GetMethod("TryUnlink")!;
            _clear = type.GetMethod("Clear")!;
            _thread = (Thread)type.GetField("_thread", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(_instance)!;
            type.GetEvent("OutFrames")!.AddEventHandler(_instance,
                (Action<ReadOnlyMemory<byte>>)(pcm => Output.Enqueue(pcm.ToArray())));
        }

        /// <summary>Queues an ISO request with fixed 48 kHz packet sizes and optional OUT PCM.</summary>
        internal void Submit(uint sequence, bool isIn, int packets, byte[]? pcm = null)
        {
            var pending = Activator.CreateInstance(_pending)!;
            var bytes = isIn ? 192u : 384u;
            void Set(string field, object value) => _pending.GetField(field, Fields)!.SetValue(pending, value);
            Set("Seqnum", sequence);
            Set("IsIn", isIn);
            Set("Packets", Enumerable.Range(0, packets).Select(i => ((uint)i * bytes, bytes)).ToArray());
            Set("TransferBufferLength", checked(packets * (int)bytes));
            if (pcm is not null) Set("OutPayload", pcm);
            _submit.Invoke(_instance, new[] { pending });
        }

        /// <summary>Cancels a request and reports whether it was still pending.</summary>
        internal bool Unlink(uint sequence) => (bool)_unlink.Invoke(_instance, new object[] { sequence })!;
        /// <summary>Drops pending requests while retaining the engine's per-direction stream cursors.</summary>
        internal void Clear() => _clear.Invoke(_instance, null);

        /// <summary>Waits for progress and rejects completions that occurred before their deadline.</summary>
        internal Completion Next()
        {
            Require(_completed.TryTake(out var completion, TimeSpan.FromSeconds(3)), "audio completion timed out");
            Require(completion!.Sequence != uint.MaxValue, "audio completed before its deadline");
            return completion!;
        }

        /// <summary>Records data and checks the deadline at callback time, before the test consumes it.</summary>
        private void Completed<T>(T pending, byte[]? data, int perPacket)
        {
            // Assert on the calling thread via Next(), not on the SDK worker.
            var early = Stopwatch.GetTimestamp() < (long)_due.GetValue(pending)!;
            _completed.Add(new Completion(early ? uint.MaxValue : (uint)_sequence.GetValue(pending)!, data, perPacket));
        }

        /// <summary>Stops the pacing thread before releasing its test completion queue.</summary>
        public void Dispose()
        {
            _instance.Dispose();
            Require(_thread.Join(TimeSpan.FromSeconds(3)), "audio pump did not stop");
            _completed.Dispose();
        }
    }
}
