// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.InputSystem.Layouts;
using UnityEngine.InputSystem.LowLevel;
using UnityEngine.InputSystem.Utilities;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestBridge.Input
{
    [StructLayout(LayoutKind.Sequential)]
    public struct QuestBridgeHMDInputState : IInputStateTypeInfo
    {
        public static FourCC Format => new FourCC('Q', 'L', 'H', 'M');
        public FourCC format => Format;

        [InputControl(layout = "Integer")] public int trackingState;
        [InputControl(layout = "Button")] public float isTracked;
        [InputControl(noisy = true, dontReset = true)] public Vector3 devicePosition;
        [InputControl(noisy = true, dontReset = true)] public Quaternion deviceRotation;
        [InputControl(noisy = true)] public Vector3 centerEyePosition;
        [InputControl(noisy = true)] public Quaternion centerEyeRotation;
        [InputControl(noisy = true)] public Vector3 leftEyePosition;
        [InputControl(noisy = true)] public Quaternion leftEyeRotation;
        [InputControl(noisy = true)] public Vector3 rightEyePosition;
        [InputControl(noisy = true)] public Quaternion rightEyeRotation;
    }

    // Quest headset over Link. Bind like any XR HMD: <XRHMD>/centerEyePosition etc.
    [InputControlLayout(stateType = typeof(QuestBridgeHMDInputState), displayName = "QuestBridge HMD", updateBeforeRender = true)]
    public class QuestBridgeHMD : XRHMD
    {
    }
}
