using System;
using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Hand tracking from the rig's backend. Put it on the QuestLinkRig object (or a child). Joint
    // poses are in tracking space (the rig transform), Unity convention; refreshed in Update and
    // again just before rendering. Hands flow once libquestlink turned them on (QL_HANDS=0 disables).
    [DisallowMultipleComponent]
    public class QuestLinkHands : MonoBehaviour
    {
        [Tooltip("Rig whose backend supplies the hands; defaults to the QuestLinkRig on this object or a parent.")]
        public QuestLinkRig rig;

        // Most recently enabled instance (the XR Hands provider reads it)
        public static QuestLinkHands Active { get; private set; }

        public QuestLinkHandState Left { get; } = new QuestLinkHandState(QuestLinkHand.Left);
        public QuestLinkHandState Right { get; } = new QuestLinkHandState(QuestLinkHand.Right);
        // Backend has delivered hand data (both may still be untracked)
        public bool HasData { get; private set; }

        // After every refresh, so visuals can follow the newest joints
        public event Action Updated;

        public QuestLinkHandState Get(QuestLinkHand hand) => hand == QuestLinkHand.Left ? Left : Right;

        public Transform TrackingSpace => rig != null ? rig.transform : transform;

        public Pose WorldJoint(QuestLinkHand hand, QuestLinkHandJoint joint)
        {
            var j = Get(hand)[joint];
            var space = TrackingSpace;
            return new Pose(space.TransformPoint(j.position), space.rotation * j.rotation);
        }

        void OnEnable()
        {
            if (rig == null)
                rig = GetComponentInParent<QuestLinkRig>();
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
            var backend = rig != null ? rig.Backend : null;
            if (backend == null)
            {
                HasData = false;
                Left.Clear();
                Right.Clear();
            }
            else
            {
                HasData = backend.TryGetHand(QuestLinkHand.Left, Left);
                backend.TryGetHand(QuestLinkHand.Right, Right);
            }
            Updated?.Invoke();
        }
    }
}
