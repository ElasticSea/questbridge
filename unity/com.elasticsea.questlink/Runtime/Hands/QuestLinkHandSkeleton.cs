using UnityEngine;

namespace ElasticSea.QuestLink
{
    // C# port of libquestlink's hand forward kinematics (native/src/hands.cpp), for the mock
    // backend and tests. Works in raw Link convention (right-handed, -Z forward); Unity's
    // Quaternion operators are plain quaternion algebra, so they apply unchanged.
    //
    // OVR hand bones (24): 0 WristRoot, 1 ForearmStub, 2-5 Thumb0-3, 6-8 Index1-3, 9-11 Middle1-3,
    // 12-14 Ring1-3, 15-18 Pinky0-3, 19-23 tips. Left bones point along +X with the back of the
    // hand at -Y; the right hand is mirrored (-X, +Y).
    public static class QuestLinkHandSkeleton
    {
        public const int BoneCount = 24;

        public static readonly int[] Parents = { -1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18 };

        // OpenXR joint -> OVR bone; -1 = synthesized (palm, index/middle/ring metacarpals)
        public static readonly int[] JointBone = { -1, 0, 3, 4, 5, 19, -1, 6, 7, 8, 20, -1, 9, 10, 11, 21, -1, 12, 13, 14, 22, 15, 16, 17, 18, 23 };

        // The bench user's left hand (hands fixture topic 9): bind rotation and translation
        // local to the parent. Right hand = translations negated.
        static readonly float[,] Bind =
        {
            { 0, 0, 0, 1, 0, 0, 0 },
            { 0, 0, 0, 1, 0, 0, 0 },
            { 0.375386924f, 0.424584061f, -0.00777885597f, 0.8238644f, 0.020069303f, 0.0115540996f, -0.0104965204f },
            { 0.260230273f, 0.0243308842f, 0.125678018f, 0.957023084f, 0.0248525608f, 0, 0 },
            { -0.0827037692f, -0.0769616961f, -0.0840622336f, 0.990035653f, 0.0325129107f, 0, 0 },
            { 0.0835059285f, 0.0650157332f, -0.0582740605f, 0.992675185f, 0.0337930955f, 0, 0 },
            { 0.0306830909f, -0.0188555885f, 0.0432814434f, 0.998413622f, 0.0959962383f, 0.00731645525f, -0.0235506799f },
            { -0.0258524101f, -0.0071160607f, 0.00329294428f, 0.999635041f, 0.0379273035f, 0, 0 },
            { -0.0160560012f, -0.027148718f, -0.0720340014f, 0.99690336f, 0.0243036486f, 0, 0 },
            { -0.00906632561f, -0.0514655858f, 0.0518357493f, 0.997287393f, 0.0956466123f, 0.00254315487f, -0.00172590557f },
            { -0.0112282336f, -0.00437887385f, -0.00197826698f, 0.999925435f, 0.0429270007f, 0, 0 },
            { -0.0343195461f, -0.00461183861f, -0.0930070132f, 0.995063126f, 0.027549576f, 0, 0 },
            { -0.0531593636f, -0.123103403f, 0.0498134904f, 0.989716232f, 0.0886937976f, 0.00652930792f, 0.0174652431f },
            { -0.0336325206f, -0.0027898401f, 0.00567601994f, 0.999414265f, 0.0389961042f, 0, 0 },
            { -0.00347746164f, 0.0291794538f, -0.0250285417f, 0.999254763f, 0.0265733916f, 0, 0 },
            { -0.207035989f, -0.140342847f, 0.0183118004f, 0.968041658f, 0.0340735577f, 0.0094198361f, 0.0229985751f },
            { 0.0911130384f, 0.00407136977f, 0.0281292256f, 0.99543488f, 0.0456505492f, 9.97678967e-07f, -2.19396293e-06f },
            { -0.0376166478f, -0.0429377183f, -0.0132860504f, 0.998280942f, 0.0307204165f, 0, 0 },
            { 0.000644743443f, 0.0491706654f, -0.0240188278f, 0.99850136f, 0.0203113798f, 0, 0 },
            { 0, 0, 0, 1, 0.0245907661f, -0.00102697383f, 0.000670370122f },
            { 0, 0, 0, 1, 0.0223633796f, -0.00102507032f, 0.000295607577f },
            { 0, 0, 0, 1, 0.0249649212f, -0.00113729888f, 0.000308652816f },
            { 0, 0, 0, 1, 0.0243261252f, -0.0016081715f, 0.000257904961f },
            { 0, 0, 0, 1, 0.0219223797f, -0.00121608598f, -0.000246479642f },
        };

        // Capsule radius per bone, derived as in hands.cpp bone_radii()
        static readonly float[] Radius =
        {
            0.0202523f, 0.0202523f, 0.0183825f, 0.0183825f, 0.0102830f, 0.0097688f,
            0.0102953f, 0.0080381f, 0.0076362f, 0.0111739f, 0.0080310f, 0.0076294f,
            0.0099221f, 0.0076117f, 0.0072311f, 0.0084834f, 0.0084834f, 0.0067642f, 0.0064260f,
            0.0097688f, 0.0076362f, 0.0076294f, 0.0072311f, 0.0064260f,
        };

        // OVR bone frame -> OpenXR joint frame (-Z along the bone, +Y back of the hand)
        static readonly Quaternion[] ToOpenXr =
        {
            new Quaternion(0.70710678f, 0f, -0.70710678f, 0f),
            new Quaternion(0f, 0.70710678f, 0f, 0.70710678f),
        };

        public static Quaternion BindRotation(int bone) => new Quaternion(Bind[bone, 0], Bind[bone, 1], Bind[bone, 2], Bind[bone, 3]);

        public static Vector3 BindPosition(QuestLinkHand hand, int bone)
        {
            var p = new Vector3(Bind[bone, 4], Bind[bone, 5], Bind[bone, 6]);
            return hand == QuestLinkHand.Left ? p : -p;
        }

        public static float BoneRadius(int bone) => Radius[bone];

        // OVR WristRoot rotation whose OpenXR wrist frame is openXrWrist (both Link convention)
        public static Quaternion RootFromWrist(QuestLinkHand hand, Quaternion openXrWrist) =>
            openXrWrist * Quaternion.Inverse(ToOpenXr[(int)hand]);

        // World (tracking space, Link convention) poses of the 24 OVR bones: per bone
        // parent * (bind translation * scale, local rotation), from the root (wrist) pose.
        public static void Bones(QuestLinkHand hand, Pose root, Quaternion[] local, float scale, Pose[] world)
        {
            for (var b = 0; b < BoneCount; b++)
            {
                var p = Parents[b];
                var parent = p < 0 ? root : world[p];
                world[b] = new Pose(parent.position + parent.rotation * (BindPosition(hand, b) * scale), parent.rotation * local[b]);
            }
        }

        // The 26 OpenXR joints in Link convention with OpenXR axes, like ql_hand.joints.
        public static void Joints(QuestLinkHand hand, Pose[] bones, float scale, QuestLinkJointPose[] joints)
        {
            var fix = ToOpenXr[(int)hand];
            for (var j = 0; j < QuestLinkHandState.JointCount; j++)
            {
                var b = JointBone[j];
                if (b >= 0)
                    joints[j] = new QuestLinkJointPose { position = bones[b].position, rotation = bones[b].rotation * fix, radius = Radius[b] * scale };
            }
            // Index/middle/ring metacarpals a third of the way from the wrist to the knuckle,
            // pointing at it; palm at the centre of the middle metacarpal
            var wrist = joints[(int)QuestLinkHandJoint.Wrist];
            var up = wrist.rotation * Vector3.up;
            foreach (var m in new[] { QuestLinkHandJoint.IndexMetacarpal, QuestLinkHandJoint.MiddleMetacarpal, QuestLinkHandJoint.RingMetacarpal })
            {
                var knuckle = joints[(int)m + 1];
                var p = wrist.position + (knuckle.position - wrist.position) / 3f;
                joints[(int)m] = new QuestLinkJointPose { position = p, rotation = Look(knuckle.position - p, up), radius = knuckle.radius };
            }
            var mm = joints[(int)QuestLinkHandJoint.MiddleMetacarpal];
            joints[(int)QuestLinkHandJoint.Palm] = new QuestLinkJointPose
            {
                position = (mm.position + joints[(int)QuestLinkHandJoint.MiddleProximal].position) * 0.5f,
                rotation = mm.rotation,
                radius = Radius[0] * scale,
            };
        }

        // OpenXR frame (right-handed algebra) looking along forward (-Z), +Y toward up
        static Quaternion Look(Vector3 forward, Vector3 up)
        {
            var z = -forward.normalized;
            var x = Vector3.Cross(up, z).normalized;  // right-handed cross product
            var y = Vector3.Cross(z, x);
            return FromAxes(x, y, z);
        }

        static Quaternion FromAxes(Vector3 x, Vector3 y, Vector3 z)
        {
            var tr = x.x + y.y + z.z;
            if (tr > 0f)
            {
                var s = Mathf.Sqrt(tr + 1f) * 2f;
                return new Quaternion((y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4f);
            }
            if (x.x > y.y && x.x > z.z)
            {
                var s = Mathf.Sqrt(1f + x.x - y.y - z.z) * 2f;
                return new Quaternion(s / 4f, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s);
            }
            if (y.y > z.z)
            {
                var s = Mathf.Sqrt(1f + y.y - x.x - z.z) * 2f;
                return new Quaternion((y.x + x.y) / s, s / 4f, (z.y + y.z) / s, (z.x - x.z) / s);
            }
            {
                var s = Mathf.Sqrt(1f + z.z - x.x - y.y) * 2f;
                return new Quaternion((z.x + x.z) / s, (z.y + y.z) / s, s / 4f, (x.y - y.x) / s);
            }
        }

        // Link-convention joints -> Unity convention (in place), as NativeQuestLink does
        public static void ToUnity(QuestLinkJointPose[] joints)
        {
            for (var j = 0; j < joints.Length; j++)
            {
                joints[j].position = QuestLinkMath.LinkToUnity(joints[j].position);
                joints[j].rotation = QuestLinkMath.LinkToUnity(joints[j].rotation);
            }
        }
    }
}
