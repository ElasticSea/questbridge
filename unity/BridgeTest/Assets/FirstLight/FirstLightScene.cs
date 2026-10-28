using ElasticSea.QuestLink.XRHands;
using UnityEngine;
using UnityEngine.Rendering;

namespace ElasticSea.QuestLink.BridgeTest
{
    // First live scene for the headset: sky colour, a 1 m floor grid, cubes 1-3 m away and big
    // instructions, rendered through a QuestLinkRig on the real libquestlink backend. On the
    // first head pose the rig is recentred so the instructions are straight ahead and, if the
    // Link origin is at eye level, the floor drops below the head. Tracked hands show as joint
    // spheres (QuestLinkHands + HandJointsVisualizer), also fed to Unity XR Hands; "-hands" shows
    // the hand instructions instead of the audio test. "-sequence" runs BenchSequence (guided
    // volume, audio, hands and yes/no questions on the board), "-sequence-audio" its audio part
    // plus a handover to the next host, "-sequence-controllers" a controller and haptics pass
    // answered with the controllers; "-mock" uses the mock backend.
    public class FirstLightScene : MonoBehaviour
    {
        const string StaleHint = "Headset is still in an old Link session (poses arrive, no handshake): quit Link in the headset and press Launch again.";
        const string Instructions = "Look around:\nobjects should stay still, in 3D.\n\nHold up both hands, open and\nclose them and pinch:\nspheres follow your fingers.\n\nThen take the headset off.";
        const string AudioInstructions = "Look around:\nobjects should stay still, in 3D.\n\nAudio test below.\nThen take the headset off.";

        public QuestLinkRig.BackendMode backend = QuestLinkRig.BackendMode.Native;
        public Color sky = new Color(0.55f, 0.75f, 0.95f);
        [Tooltip("Head height used when the Link origin is at eye level.")]
        public float standingHeight = 1.6f;
        [Tooltip("Game audio to the headset (beeps, mic playback) and the headset mic back; AudioBench.")]
        public bool audioTest = true;

        QuestLinkRig rig;
        QuestLinkHands hands;
        QuestLinkXRHands xrHands;
        bool recentred;
        int submitted;
        QuestLinkState lastState = QuestLinkState.Idle;
        float connectingSince = -1f;
        bool staleLogged;
        GUIStyle statusStyle;
        bool sequence, audioOnly, controllersOnly;
        AudioBench bench;
        QuestLinkAudio output;
        TextMesh board;

        void Awake()
        {
            // "-native-check": log whether libquestlink loads, then quit (exit 1 if not). No USB.
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-native-check") >= 0)
            {
                var ok = NativeQuestLink.LibraryLoads(out var error);
                Debug.Log(ok ? "[QuestLink] native check: libquestlink loaded" : $"[QuestLink] native check failed: {error}");
                Application.Quit(ok ? 0 : 1);
                enabled = false;
                return;
            }

            // Stream at the headset's rate even while the Editor is not focused; never hold the cursor
            Application.runInBackground = true;
            Cursor.lockState = CursorLockMode.None;
            Cursor.visible = true;
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 72;
            // Player builds: "-seconds N" quits after N s so the bench gets USB back
            var seconds = SecondsArg();
            if (seconds > 0f)
            {
                Debug.Log($"[QuestLink] quitting after {seconds} s");
                Invoke(nameof(Quit), seconds);
            }
            // "-hands": hand-tracking bench (hand instructions instead of the audio test)
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-hands") >= 0)
                audioTest = false;
            // "-sequence": BenchSequence, once, with the steps and questions on the board
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-sequence") >= 0)
                sequence = audioTest = true;
            // "-sequence-audio": volume + audio + questions, then hand the session to the next host
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-sequence-audio") >= 0)
                sequence = audioTest = audioOnly = true;
            // "-sequence-controllers": controllers, buttons, analog, motion and haptics; no audio
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-sequence-controllers") >= 0)
                sequence = audioTest = controllersOnly = true;
            if (System.Array.IndexOf(System.Environment.GetCommandLineArgs(), "-mock") >= 0)
                backend = QuestLinkRig.BackendMode.Mock;

            var screen = new GameObject("Screen Camera").AddComponent<Camera>();
            screen.clearFlags = CameraClearFlags.SolidColor;
            screen.backgroundColor = Color.black;
            screen.cullingMask = 0;
            screen.depth = -100;

            var light = new GameObject("Sun").AddComponent<Light>();
            light.type = LightType.Directional;
            light.transform.rotation = Quaternion.Euler(50, -30, 0);
            RenderSettings.ambientMode = AmbientMode.Flat;
            RenderSettings.ambientLight = new Color(0.5f, 0.55f, 0.6f);

            Floor();
            Cube(new Vector3(0f, 0.25f, 1.5f), 0.5f, new Color(0.9f, 0.3f, 0.25f));
            Cube(new Vector3(-1.2f, 0.4f, 2.2f), 0.8f, new Color(0.25f, 0.7f, 0.35f));
            Cube(new Vector3(1.3f, 0.3f, 2.0f), 0.6f, new Color(0.25f, 0.45f, 0.9f));
            Cube(new Vector3(-0.6f, 1.2f, 3.0f), 0.3f, new Color(0.95f, 0.8f, 0.2f));
            Cube(new Vector3(2.2f, 0.5f, -0.5f), 1.0f, new Color(0.8f, 0.4f, 0.85f));
            Cube(new Vector3(-2.0f, 0.35f, -1.0f), 0.7f, new Color(0.95f, 0.6f, 0.2f));
            Cube(new Vector3(0.3f, 0.2f, -2.0f), 0.4f, new Color(0.3f, 0.85f, 0.85f));
            board = Text(sequence ? "Starting soon..." : audioTest ? AudioInstructions : Instructions, new Vector3(0f, sequence ? 2.1f : 1.9f, 3.2f), sequence ? 1.7f : audioTest ? 1.2f : 2.3f);

            var rigObject = new GameObject("QuestLink Rig");
            rigObject.SetActive(false);
            rig = rigObject.AddComponent<QuestLinkRig>();
            rig.backend = backend;
            hands = rigObject.AddComponent<QuestLinkHands>();
            rigObject.AddComponent<HandJointsVisualizer>();
            xrHands = rigObject.AddComponent<QuestLinkXRHands>();
            rigObject.SetActive(true);
            if (rig.Backend != null)
                Debug.Log($"[QuestLink] backend {rig.Backend.Name}, {SystemInfo.graphicsDeviceType}");
            if (audioTest && rig.Head != null)
                AudioTest();
            if (sequence && bench != null)
            {
                var seq = gameObject.AddComponent<BenchSequence>();
                seq.rig = rig;
                seq.hands = hands;
                seq.bench = bench;
                seq.output = output;
                seq.board = board;
                seq.audioOnly = audioOnly;
                seq.controllersOnly = controllersOnly;
            }
        }

        // Listener on the head, sent to the headset; beeps and mic record/playback below the instructions
        void AudioTest()
        {
            output = rig.Head.gameObject.AddComponent<QuestLinkAudio>();
            output.rig = rig;
            bench = new GameObject("Audio Bench").AddComponent<AudioBench>();
            bench.once = sequence;
            var mic = bench.gameObject.AddComponent<QuestLinkMicrophone>();
            mic.rig = rig;
            bench.output = output;
            bench.mic = mic;
            bench.label = Text("", new Vector3(0f, 0.7f, 3.2f), 0.9f);
            Debug.Log($"[QuestLink] audio test on, output {AudioSettings.outputSampleRate} Hz {AudioSettings.speakerMode}");
        }

        void Update()
        {
            if (rig == null || rig.Backend == null)
                return;
            foreach (var eye in new[] { rig.LeftEye, rig.RightEye })
            {
                eye.clearFlags = CameraClearFlags.SolidColor;
                eye.backgroundColor = sky;
            }
            var hasPose = rig.Backend.TryGetHeadPose(out var head);
            if (!recentred && hasPose)
                Recentre(head);
            TrackState(hasPose);
        }

        // Log every state change; poses while still Connecting mean the headset kept an old
        // session (our runtime waits for a fresh HELLO), which only Quit Link + Launch fixes.
        void TrackState(bool hasPose)
        {
            var state = rig.State;
            if (state != lastState)
            {
                Debug.Log($"[QuestLink] state {lastState} -> {state} at {Time.realtimeSinceStartup:F1} s");
                lastState = state;
                connectingSince = state == QuestLinkState.Connecting ? Time.realtimeSinceStartup : -1f;
                staleLogged = false;
            }
            if (!staleLogged && Stale(hasPose))
            {
                staleLogged = true;
                Debug.Log($"[QuestLink] {StaleHint}");
            }
        }

        bool Stale(bool hasPose) =>
            hasPose && rig.State == QuestLinkState.Connecting && connectingSince >= 0f &&
            Time.realtimeSinceStartup - connectingSince > 5f;

        // PC window only: the headset shows our frames only once Streaming
        void OnGUI()
        {
            if (rig == null || rig.Backend == null)
                return;
            GUI.depth = -10; // above the rig's mirror
            statusStyle ??= new GUIStyle(GUI.skin.label) { fontSize = 22, wordWrap = true, normal = { textColor = Color.yellow } };
            var text = $"QuestLink: {rig.State}, frame {rig.FrameIndex}";
            if (staleLogged && rig.State == QuestLinkState.Connecting)
                text += "\n" + StaleHint;
            GUI.Label(new Rect(16, 12, Screen.width - 32, 120), text, statusStyle);
        }

        void LateUpdate()
        {
            if (rig != null && rig.FrameIndex > 0 && (int)rig.FrameIndex / 720 != submitted)
            {
                submitted = (int)rig.FrameIndex / 720;
                Debug.Log($"[QuestLink] {rig.State}, frame {rig.FrameIndex}, head {rig.RenderPose.position}");
                Debug.Log($"[QuestLink] hands data {hands.HasData}: {Describe(hands.Left)}; {Describe(hands.Right)}; " +
                          $"XR Hands tracked L {xrHands.Subsystem?.leftHand.isTracked} R {xrHands.Subsystem?.rightHand.isTracked}");
            }
        }

        static string Describe(QuestLinkHandState h) => h.tracked
            ? $"{h.hand} tracked conf {h.confidence:F2} pinch {h.Pinch(QuestLinkFinger.Index):F2} wrist {h[QuestLinkHandJoint.Wrist].position} t {h.timeNs}"
            : $"{h.hand} not tracked";

        static float SecondsArg()
        {
            var args = System.Environment.GetCommandLineArgs();
            for (var i = 0; i + 1 < args.Length; i++)
                if (args[i] == "-seconds" && float.TryParse(args[i + 1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var s))
                    return s;
            return 0f;
        }

        void Quit()
        {
            Debug.Log($"[QuestLink] quit: {(rig != null ? rig.State.ToString() : "no rig")}, frame {(rig != null ? rig.FrameIndex : 0)}");
            // Leaves the headset's session up and logs the resume numbers for an attaching host
            if (rig != null && rig.Backend is NativeQuestLink)
                NativeQuestLink.Shutdown();
            Application.Quit();
        }

        // Turn the rig so the current head yaw faces +Z and put the head over the origin.
        void Recentre(QuestLinkPose head)
        {
            recentred = true;
            var yaw = Quaternion.Euler(0f, -head.rotation.eulerAngles.y, 0f);
            var p = yaw * head.position;
            var eyeLevel = head.position.y < 0.8f;
            var y = eyeLevel ? standingHeight - head.position.y : 0f;
            rig.transform.SetPositionAndRotation(new Vector3(-p.x, y, -p.z), yaw);
            Debug.Log($"[QuestLink] first head pose {head.position}, yaw {head.rotation.eulerAngles.y:F0}; " +
                      (eyeLevel ? $"eye-level origin, head placed at {standingHeight} m" : "floor-level origin"));
        }

        static void Floor()
        {
            const int size = 40;
            var floor = GameObject.CreatePrimitive(PrimitiveType.Quad);
            floor.name = "Floor Grid";
            floor.transform.rotation = Quaternion.Euler(90, 0, 0);
            floor.transform.localScale = new Vector3(size, size, 1);
            // Copy of the primitive's default material: Shader.Find'd shaders get stripped from players
            var renderer = floor.GetComponent<Renderer>();
            var mat = new Material(renderer.sharedMaterial) { mainTexture = GridTexture() };
            mat.mainTextureScale = new Vector2(size, size);
            renderer.sharedMaterial = mat;
        }

        // One 1 m tile: dark grey with a light border line
        static Texture2D GridTexture()
        {
            const int n = 256, line = 4;
            var tex = new Texture2D(n, n, TextureFormat.RGBA32, true) { wrapMode = TextureWrapMode.Repeat, anisoLevel = 8, filterMode = FilterMode.Trilinear };
            var px = new Color32[n * n];
            for (var y = 0; y < n; y++)
            for (var x = 0; x < n; x++)
            {
                var edge = x < line || y < line || x >= n - line || y >= n - line;
                px[y * n + x] = edge ? new Color32(230, 230, 230, 255) : new Color32(70, 75, 80, 255);
            }
            tex.SetPixels32(px);
            tex.Apply(true);
            return tex;
        }

        static void Cube(Vector3 position, float size, Color color)
        {
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.transform.position = position;
            cube.transform.localScale = Vector3.one * size;
            cube.GetComponent<Renderer>().material.color = color;
        }

        static TextMesh Text(string text, Vector3 position, float boardHeight)
        {
            var go = new GameObject("Instructions");
            go.transform.position = position;
            var mesh = go.AddComponent<TextMesh>();
            var font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            mesh.font = font;
            mesh.text = text;
            mesh.fontSize = 96;
            mesh.characterSize = 0.012f; // ~12 cm capitals at 3 m
            mesh.anchor = TextAnchor.MiddleCenter;
            mesh.alignment = TextAlignment.Center;
            mesh.color = Color.white;
            go.GetComponent<MeshRenderer>().sharedMaterial = font.material;

            // Dark backing board so the text reads against the sky
            var board = GameObject.CreatePrimitive(PrimitiveType.Quad);
            board.name = "Board";
            board.transform.SetParent(go.transform, false);
            board.transform.localPosition = new Vector3(0f, 0f, 0.02f);
            board.transform.localScale = new Vector3(3.0f, boardHeight, 1f);
            board.GetComponent<Renderer>().material.color = new Color(0.1f, 0.12f, 0.16f);
            return mesh;
        }
    }
}
