using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestLink.Samples
{
    // Builds the demo at Play: a QuestLinkRig, a floor, a ring of cubes and two controller cubes
    // driven by stock TrackedPoseDrivers bound to generic XR paths. Without libquestlink the rig
    // falls back to the mock backend, so the head looks around on its own.
    public class BasicSceneSetup : MonoBehaviour
    {
        public QuestLinkRig.BackendMode backend = QuestLinkRig.BackendMode.Auto;

        void Awake()
        {
            var light = new GameObject("Light").AddComponent<Light>();
            light.type = LightType.Directional;
            light.transform.rotation = Quaternion.Euler(50, -30, 0);

            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            floor.transform.localScale = new Vector3(2, 1, 2);

            for (var i = 0; i < 12; i++)
            {
                var a = i * Mathf.PI * 2 / 12;
                var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
                cube.name = $"Cube {i}";
                cube.transform.position = new Vector3(Mathf.Sin(a) * 3, 1.2f + 0.3f * (i % 3), Mathf.Cos(a) * 3);
                cube.transform.localScale = Vector3.one * 0.4f;
                cube.GetComponent<Renderer>().material.color = Color.HSVToRGB(i / 12f, 0.7f, 0.9f);
            }

            var rigObject = new GameObject("QuestLink Rig");
            rigObject.SetActive(false);
            var rig = rigObject.AddComponent<QuestLinkRig>();
            rig.backend = backend;
            rigObject.SetActive(true);

            Hand(rigObject.transform, "Left Controller", "LeftHand");
            Hand(rigObject.transform, "Right Controller", "RightHand");
        }

        static void Hand(Transform origin, string name, string usage)
        {
            var hand = GameObject.CreatePrimitive(PrimitiveType.Cube);
            hand.name = name;
            hand.transform.SetParent(origin, false);
            hand.transform.localScale = new Vector3(0.05f, 0.05f, 0.15f);
            Destroy(hand.GetComponent<Collider>());

            var driver = hand.AddComponent<TrackedPoseDriver>();
            driver.positionInput = new InputActionProperty(new InputAction(binding: $"<XRController>{{{usage}}}/devicePosition"));
            driver.rotationInput = new InputActionProperty(new InputAction(binding: $"<XRController>{{{usage}}}/deviceRotation"));
        }
    }
}
