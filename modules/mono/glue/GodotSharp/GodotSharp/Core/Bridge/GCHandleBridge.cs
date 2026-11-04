using System;
using System.Runtime.InteropServices;
using Godot.NativeInterop;

namespace Godot.Bridge
{
    internal static class GCHandleBridge
    {
        // Native holds the binding handle lock until this independent copy is made.
        [UnmanagedCallersOnly]
        internal static unsafe IntPtr AllocStrongGCHandle(IntPtr gcHandlePtr, godot_bool* targetAlive)
        {
            *targetAlive = godot_bool.False;
            try
            {
                object target = GCHandle.FromIntPtr(gcHandlePtr).Target;
                if (target == null)
                    return IntPtr.Zero;

                *targetAlive = godot_bool.True;
                return GCHandle.ToIntPtr(GCHandle.Alloc(target, GCHandleType.Normal));
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
                return IntPtr.Zero;
            }
        }

        [UnmanagedCallersOnly]
        internal static void FreeGCHandle(IntPtr gcHandlePtr)
        {
            try
            {
                CustomGCHandle.Free(GCHandle.FromIntPtr(gcHandlePtr));
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
            }
        }

        // Returns true, if releasing the provided handle is necessary for assembly unloading to succeed.
        // This check is not perfect and only intended to prevent things in GodotTools from being reloaded.
        [UnmanagedCallersOnly]
        internal static godot_bool GCHandleIsTargetCollectible(IntPtr gcHandlePtr)
        {
            try
            {
                var target = GCHandle.FromIntPtr(gcHandlePtr).Target;

                if (target is Delegate @delegate)
                    return DelegateUtils.IsDelegateCollectible(@delegate).ToGodotBool();

                return target.GetType().IsCollectible.ToGodotBool();
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
                return godot_bool.True;
            }
        }
    }
}
