using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Draws every hand joint as a sphere of its radius under the tracking space, hidden while the
    // hand is untracked. Thumb and index tips light up with the index pinch.
    public class HandJointsVisualizer : MonoBehaviour
    {
        [Tooltip("Defaults to the QuestLinkHands on this object or a parent.")]
        public QuestLinkHands hands;
        public Color leftColor = new Color(0.95f, 0.75f, 0.55f);
        public Color rightColor = new Color(0.55f, 0.8f, 0.95f);
        public Color pinchColor = new Color(1f, 0.25f, 0.2f);
        [Tooltip("Sphere diameter = joint radius x 2 x this.")]
        public float radiusScale = 1f;

        Transform root;
        readonly Transform[][] spheres = new Transform[2][];
        readonly Material[] handMaterials = new Material[2];
        readonly Material[] tipMaterials = new Material[2];

        public Transform JointSphere(QuestLinkHand hand, QuestLinkHandJoint joint) => spheres[(int)hand]?[(int)joint];

        void OnEnable()
        {
            if (hands == null)
                hands = GetComponentInParent<QuestLinkHands>();
            if (hands == null)
            {
                Debug.LogError("[QuestLink] HandJointsVisualizer needs a QuestLinkHands.", this);
                enabled = false;
                return;
            }
            Build();
            hands.Updated += Apply;
            Apply();
        }

        void OnDisable()
        {
            if (hands != null)
                hands.Updated -= Apply;
            if (root != null)
                Release(root.gameObject);
            foreach (var m in handMaterials) Release(m);
            foreach (var m in tipMaterials) Release(m);
            root = null;
        }

        void Build()
        {
            root = new GameObject("Hand Joints").transform;
            root.SetParent(hands.TrackingSpace, false);
            for (var h = 0; h < 2; h++)
            {
                spheres[h] = new Transform[QuestLinkHandState.JointCount];
                for (var j = 0; j < QuestLinkHandState.JointCount; j++)
                {
                    var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                    go.name = $"{(QuestLinkHand)h} {(QuestLinkHandJoint)j}";
                    Release(go.GetComponent<Collider>());
                    go.transform.SetParent(root, false);
                    var r = go.GetComponent<Renderer>();
                    // Copies of the primitive's material: Shader.Find'd shaders get stripped from players
                    if (handMaterials[h] == null)
                    {
                        handMaterials[h] = new Material(r.sharedMaterial) { color = h == 0 ? leftColor : rightColor };
                        tipMaterials[h] = new Material(handMaterials[h]);
                    }
                    var tip = j == (int)QuestLinkHandJoint.ThumbTip || j == (int)QuestLinkHandJoint.IndexTip;
                    r.sharedMaterial = tip ? tipMaterials[h] : handMaterials[h];
                    spheres[h][j] = go.transform;
                }
            }
        }

        void Apply()
        {
            if (root == null)
                return;
            for (var h = 0; h < 2; h++)
            {
                var state = hands.Get((QuestLinkHand)h);
                for (var j = 0; j < QuestLinkHandState.JointCount; j++)
                {
                    var t = spheres[h][j];
                    if (t.gameObject.activeSelf != state.tracked)
                        t.gameObject.SetActive(state.tracked);
                    if (!state.tracked)
                        continue;
                    var joint = state.joints[j];
                    t.localPosition = joint.position;
                    t.localRotation = joint.rotation;
                    t.localScale = Vector3.one * Mathf.Max(joint.radius, 0.004f) * 2f * radiusScale;
                }
                tipMaterials[h].color = Color.Lerp(handMaterials[h].color, pinchColor, state.Pinch(QuestLinkFinger.Index));
            }
        }

        static void Release(Object o)
        {
            if (o == null)
                return;
            if (Application.isPlaying) Object.Destroy(o);
            else DestroyImmediate(o);
        }
    }
}
