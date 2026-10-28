using System;
using UnityEngine;

namespace ElasticSea.QuestBridge
{
    // Hand tracking from the rig's backend. Put it on the QuestBridgeRig object (or a child). Joint
    // poses are in tracking space (the rig transform), Unity convention; refreshed in Update and
    // again just before rendering. Hands flow once libquestbridge turned them on (QB_HANDS=0 disables).
    [DisallowMultipleComponent]
    public class QuestBridgeHands : MonoBehaviour
    {
        [Tooltip("Rig whose backend supplies the hands; defaults to the QuestBridgeRig on this object or a parent, else the XR loader's driver.")]
        public QuestBridgeRig rig;

        // Most recently enabled instance (the XR Hands provider reads it)
        public static QuestBridgeHands Active { get; private set; }

        public QuestBridgeHandState Left { get; } = new QuestBridgeHandState(QuestBridgeHand.Left);
        public QuestBridgeHandState Right { get; } = new QuestBridgeHandState(QuestBridgeHand.Right);
        // Backend has delivered hand data (both may still be untracked)
        public bool HasData { get; private set; }

        // After every refresh, so visuals can follow the newest joints
        public event Action Updated;

        public QuestBridgeHandState Get(QuestBridgeHand hand) => hand == QuestBridgeHand.Left ? Left : Right;

        public Transform TrackingSpace => rig != null ? rig.transform :
            QuestBridgeXRDriver.Active != null && QuestBridgeXRDriver.Active.TrackingSpace != null ? QuestBridgeXRDriver.Active.TrackingSpace : transform;

        public Pose WorldJoint(QuestBridgeHand hand, QuestBridgeHandJoint joint)
        {
            var j = Get(hand)[joint];
            var space = TrackingSpace;
            return new Pose(space.TransformPoint(j.position), space.rotation * j.rotation);
        }

        void OnEnable()
        {
            if (rig == null)
                rig = GetComponentInParent<QuestBridgeRig>();
            Active = this;
            Application.onBeforeRender += Refresh;
        }

        void OnDisable()
        {
            Application.onBeforeRender -= Refresh;
            if (Active == this)
                Active = null;
        }

        void Update() => Refresh();

        public void Refresh()
        {
            var backend = rig != null ? rig.Backend : QuestBridgeXRDriver.Active?.Backend;
            if (backend == null)
            {
                HasData = false;
                Left.Clear();
                Right.Clear();
            }
            else
            {
                HasData = backend.TryGetHand(QuestBridgeHand.Left, Left);
                backend.TryGetHand(QuestBridgeHand.Right, Right);
            }
            Updated?.Invoke();
        }
    }
}
