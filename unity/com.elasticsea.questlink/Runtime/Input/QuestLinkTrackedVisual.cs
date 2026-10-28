using UnityEngine;

namespace ElasticSea.QuestLink.Input
{
    // Shows this object's renderers only while the controller it follows is tracked, so an
    // unused controller doesn't sit at the tracking origin. Put it next to a TrackedPoseDriver.
    [AddComponentMenu("QuestLink/QuestLink Tracked Visual")]
    public class QuestLinkTrackedVisual : MonoBehaviour
    {
        public QuestLinkHand hand;
        [Tooltip("Without renderers of its own, add a small block at Play (created at runtime so it gets the render pipeline's default material).")]
        public bool defaultVisual = true;

        Renderer[] renderers;

        void OnEnable()
        {
            renderers = GetComponentsInChildren<Renderer>(true);
            if (renderers.Length == 0 && defaultVisual)
            {
                var block = GameObject.CreatePrimitive(PrimitiveType.Cube);
                block.name = "Controller Block";
                Destroy(block.GetComponent<Collider>());
                block.transform.SetParent(transform, false);
                block.transform.localScale = new Vector3(0.04f, 0.04f, 0.12f);
                renderers = new[] { block.GetComponent<Renderer>() };
            }
            Apply();
        }

        void LateUpdate() => Apply();

        public bool Tracked
        {
            get
            {
                var device = hand == QuestLinkHand.Left ? QuestLinkInput.LeftController : QuestLinkInput.RightController;
                return device != null && device.added && device.isTracked.isPressed;
            }
        }

        void Apply()
        {
            var tracked = Tracked;
            foreach (var r in renderers)
                if (r != null)
                    r.enabled = tracked;
        }
    }
}
