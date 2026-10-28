using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Layouts;
using UnityEngine.InputSystem.LowLevel;
using UnityEngine.InputSystem.Utilities;
using UnityEngine.InputSystem.XR;
using UnityEngine.InputSystem.XR.Haptics;

namespace ElasticSea.QuestLink.Input
{
    // Control names follow Unity's Oculus Touch layout; usages follow XR feature usages so
    // generic <XRController>{LeftHand}/{TriggerButton}-style bindings (XRI) resolve too.
    [StructLayout(LayoutKind.Sequential)]
    public struct QuestLinkControllerInputState : IInputStateTypeInfo
    {
        public static FourCC Format => new FourCC('Q', 'L', 'C', 'T');
        public FourCC format => Format;

        [InputControl(layout = "Integer")] public int trackingState;
        [InputControl(layout = "Button")] public float isTracked;
        [InputControl(noisy = true, dontReset = true)] public Vector3 devicePosition;
        [InputControl(noisy = true, dontReset = true)] public Quaternion deviceRotation;
        [InputControl(noisy = true, dontReset = true)] public Vector3 pointerPosition;
        [InputControl(noisy = true, dontReset = true)] public Quaternion pointerRotation;

        [InputControl(layout = "Axis", usage = "Trigger")] public float trigger;
        [InputControl(layout = "Axis", usage = "Grip")] public float grip;
        [InputControl(layout = "Stick", aliases = new[] { "Primary2DAxis", "Joystick" }, usage = "Primary2DAxis")] public Vector2 thumbstick;

        [InputControl(layout = "Button", aliases = new[] { "indexButton" }, usage = "TriggerButton")] public float triggerPressed;
        [InputControl(layout = "Button", aliases = new[] { "GripButton" }, usage = "GripButton")] public float gripPressed;
        [InputControl(layout = "Button", aliases = new[] { "A", "X", "Alternate" }, usage = "PrimaryButton")] public float primaryButton;
        [InputControl(layout = "Button", aliases = new[] { "B", "Y", "Primary" }, usage = "SecondaryButton")] public float secondaryButton;
        [InputControl(layout = "Button", aliases = new[] { "JoystickOrPadPressed", "primary2DAxisClick" }, usage = "Primary2DAxisClick")] public float thumbstickClicked;
        [InputControl(layout = "Button", aliases = new[] { "primary2DAxisTouch" }, usage = "Primary2DAxisTouch")] public float thumbstickTouched;
        [InputControl(layout = "Button", usage = "PrimaryTouch")] public float primaryTouched;
        [InputControl(layout = "Button", usage = "SecondaryTouch")] public float secondaryTouched;
        [InputControl(layout = "Button", aliases = new[] { "indexTouched" }, usage = "TriggerTouch")] public float triggerTouched;
        [InputControl(layout = "Button", aliases = new[] { "menuButton" }, usage = "MenuButton")] public float menu;
    }

    // Touch controller over Link. Haptics: SendImpulse (XRControllerWithRumble) or any
    // SendHapticImpulseCommand is forwarded to QuestLinkInput.hapticHandler.
    [InputControlLayout(stateType = typeof(QuestLinkControllerInputState), displayName = "Quest Link Controller",
        commonUsages = new[] { "LeftHand", "RightHand" }, updateBeforeRender = true)]
    public class QuestLinkController : XRControllerWithRumble
    {
        public QuestLinkHand Hand => usages.Contains(CommonUsages.RightHand) ? QuestLinkHand.Right : QuestLinkHand.Left;

        protected override unsafe long ExecuteCommand(InputDeviceCommand* commandPtr)
        {
            if (commandPtr->type == default(SendHapticImpulseCommand).typeStatic)
            {
                // Payload after the 8-byte header: int channel, float amplitude, float duration.
                var payload = (byte*)commandPtr + InputDeviceCommand.BaseCommandSize;
                QuestLinkInput.hapticHandler?.Invoke(Hand, *(float*)(payload + 4), *(float*)(payload + 8));
                return InputDeviceCommand.GenericSuccess;
            }
            return base.ExecuteCommand(commandPtr);
        }
    }
}
