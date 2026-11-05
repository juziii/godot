using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Godot.NativeInterop;

#nullable enable

namespace Godot
{
    internal static class DisposablesTracker
    {
        private static readonly object ShutdownLock = new();
        private static readonly List<(WeakReference<GodotObject> Key, GodotObject Target)> GodotObjectPins = new();
        private static readonly List<(WeakReference<IDisposable> Key, IDisposable Target)> OtherPins = new();
        private static bool _pinning;
        private static bool _preparing;
        private static bool _prepared;
        private static bool _draining;
        private static bool _completed;

        [UnmanagedCallersOnly]
        internal static godot_bool PrepareForShutdown()
        {
            try
            {
                PrepareForShutdownImpl();
                return godot_bool.True;
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
                return godot_bool.False;
            }
        }

        private static void PrepareForShutdownImpl()
        {
            lock (ShutdownLock)
            {
                if (_prepared)
                    return;
                if (_preparing)
                    throw new InvalidOperationException("Reentrant .NET shutdown preparation.");

                _preparing = true;
                _pinning = true;
                // Long weak references also expose targets whose finalizers were
                // queued already. Pinning does not cancel those finalizers.
                foreach (WeakReference<GodotObject> key in GodotObjectInstances.Keys)
                {
                    if (key.TryGetTarget(out GodotObject? target))
                        GodotObjectPins.Add((key, target));
                }
                foreach (WeakReference<IDisposable> key in OtherInstances.Keys)
                {
                    if (key.TryGetTarget(out IDisposable? target))
                        OtherPins.Add((key, target));
                }
            }

            try
            {
                // Never hold the registration lock (or native binding locks)
                // while a finalizer performs native cleanup and unregisters.
                GC.Collect();
                GC.WaitForPendingFinalizers();
                lock (ShutdownLock)
                    _prepared = true;
            }
            finally
            {
                lock (ShutdownLock)
                    _preparing = false;
            }
        }

        [UnmanagedCallersOnly]
        internal static godot_bool OnGodotShuttingDown()
        {
            try
            {
                OnGodotShuttingDownImpl();
                return godot_bool.True;
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
                return godot_bool.False;
            }
        }

        private static void OnGodotShuttingDownImpl()
        {
            // Fallback for hosts which did not run the normal main loop. The
            // normal prepare callback runs before native mainloop finalization.
            PrepareForShutdownImpl();
            lock (ShutdownLock)
            {
                if (_draining || _completed)
                    return;
                _draining = true;
            }

            try
            {
                DrainPinnedInstances();
            }
            finally
            {
                lock (ShutdownLock)
                    _draining = false;
            }
        }

        private static void DrainPinnedInstances()
        {
            bool isStdoutVerbose;

            try
            {
                isStdoutVerbose = OS.IsStdOutVerbose();
            }
            catch (ObjectDisposedException)
            {
                // OS singleton already disposed. Maybe OnUnloading was called twice.
                isStdoutVerbose = false;
            }

            if (isStdoutVerbose)
                GD.Print("Unloading: Disposing tracked instances...");

            // Dispose Godot Objects first, and only then dispose other disposables
            // like StringName, NodePath, Godot.Collections.Array/Dictionary, etc.
            // The Godot Object Dispose() method may need any of the later instances.

            int godotIndex = 0;
            int otherIndex = 0;
            bool printedFinished = false;
            while (true)
            {
                while (true)
                {
                    GodotObject? godot = null;
                    IDisposable? other = null;
                    lock (ShutdownLock)
                    {
                        // Dispose callbacks may register more wrappers. New
                        // GodotObjects take precedence over the next buffer.
                        if (godotIndex < GodotObjectPins.Count)
                        {
                            var entry = GodotObjectPins[godotIndex++];
                            // A previously queued finalizer may have completed
                            // during the join. Do not double-Unregister buffers
                            // or invoke an already completed Dispose again.
                            if (GodotObjectInstances.ContainsKey(entry.Key))
                                godot = entry.Target;
                        }
                        else if (otherIndex < OtherPins.Count)
                        {
                            var entry = OtherPins[otherIndex++];
                            if (OtherInstances.ContainsKey(entry.Key))
                                other = entry.Target;
                        }
                        else
                            break;
                    }
                    // User/native disposal and any reentrant registration run
                    // outside ShutdownLock. Pins remain until the whole drain.
                    godot?.Dispose();
                    other?.Dispose();
                }

                // Logging may itself create tracked temporary instances. Do it
                // once before the final join/stability check, never after pins
                // are cleared, so those instances also pass through the drain.
                if (isStdoutVerbose && !printedFinished)
                {
                    printedFinished = true;
                    GD.Print("Unloading: Finished disposing tracked instances.");
                }
                GC.Collect();
                GC.WaitForPendingFinalizers();
                lock (ShutdownLock)
                {
                    if (godotIndex != GodotObjectPins.Count || otherIndex != OtherPins.Count)
                        continue;
                    _completed = true;
                    GodotObjectPins.Clear();
                    OtherPins.Clear();
                    break;
                }
            }
        }

        private static ConcurrentDictionary<WeakReference<GodotObject>, byte> GodotObjectInstances { get; } =
            new();

        private static ConcurrentDictionary<WeakReference<IDisposable>, byte> OtherInstances { get; } =
            new();

        public static WeakReference<GodotObject> RegisterGodotObject(GodotObject godotObject)
        {
            var weakReferenceToSelf = new WeakReference<GodotObject>(godotObject, trackResurrection: true);
            lock (ShutdownLock)
            {
                GodotObjectInstances.TryAdd(weakReferenceToSelf, 0);
                if (_pinning)
                {
                    _completed = false;
                    GodotObjectPins.Add((weakReferenceToSelf, godotObject));
                }
            }
            return weakReferenceToSelf;
        }

        public static WeakReference<IDisposable> RegisterDisposable(IDisposable disposable)
        {
            var weakReferenceToSelf = new WeakReference<IDisposable>(disposable, trackResurrection: true);
            lock (ShutdownLock)
            {
                OtherInstances.TryAdd(weakReferenceToSelf, 0);
                if (_pinning)
                {
                    _completed = false;
                    OtherPins.Add((weakReferenceToSelf, disposable));
                }
            }
            return weakReferenceToSelf;
        }

        public static void UnregisterGodotObject(GodotObject godotObject, WeakReference<GodotObject> weakReferenceToSelf)
        {
            if (!GodotObjectInstances.TryRemove(weakReferenceToSelf, out _))
                throw new ArgumentException("Godot Object not registered.", nameof(weakReferenceToSelf));
        }

        public static void UnregisterDisposable(WeakReference<IDisposable> weakReference)
        {
            if (!OtherInstances.TryRemove(weakReference, out _))
                throw new ArgumentException("Disposable not registered.", nameof(weakReference));
        }
    }
}
