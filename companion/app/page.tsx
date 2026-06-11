/// <reference types="web-bluetooth" />
"use client";

import { useEffect, useRef, useState } from "react";

const WEARINGAID_SERVICE_UUID = "696afb11-bed7-42cc-b178-dd49bac6c8ef";
const CONFIG_CHARACTERISTIC_UUID = "692580ea-d39f-49f8-bb81-ae799d99de8d";
const HEARTBEAT_CHARACTERISTIC_UUID = "3f2b0ed6-6df7-4f1a-8d77-fc7f2f76d211";
const DEBUG_CHARACTERISTIC_UUID = "b2e7f3c1-9d4a-4f58-a6e0-3c8d12b54e71";
const FEATURES_CHARACTERISTIC_UUID = "52b0e5e0-c1a5-4bce-b68b-8a8b41b5ca5c";

const FEATURE_KEY = 1;
const FEATURE_TEMPO = 2;
const FEATURE_LISTENING = 4;

const GENRE_PROFILES = [
  { code: 0, title: "Classical", subtitle: "Jazz, traditional" },
  { code: 1, title: "Band", subtitle: "Rock, pop, live" },
  { code: 2, title: "Electronic", subtitle: "EDM, house, techno" },
  { code: 3, title: "Acoustic", subtitle: "Folk, solo, traditional" },
  { code: 4, title: "Minimal", subtitle: "Ambient, drone" },
];

const ENGINE_KEYS = [
  "C Maj","C# Maj","D Maj","D# Maj","E Maj","F Maj",
  "F# Maj","G Maj","G# Maj","A Maj","A# Maj","B Maj",
  "C Min","C# Min","D Min","D# Min","E Min","F Min",
  "F# Min","G Min","G# Min","A Min","A# Min","B Min",
];

const toArrayBuffer = (value: ArrayBufferLike | ArrayBufferView<ArrayBufferLike>): ArrayBuffer => {
  if (ArrayBuffer.isView(value)) {
    const copy = new ArrayBuffer(value.byteLength);
    new Uint8Array(copy).set(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    return copy;
  }

  const copy = new ArrayBuffer(value.byteLength);
  new Uint8Array(copy).set(new Uint8Array(value));
  return copy;
};

const isLostGattConnectionError = (error: unknown): boolean => {
  if (!(error instanceof DOMException)) {
    return false;
  }

  return (
    error.name === "InvalidStateError" ||
    error.name === "NetworkError" ||
    error.message.includes("GATT Service no longer exists") ||
    error.message.includes("disconnected")
  );
};

export default function Home() {
  const [device, setDevice] = useState<BluetoothDevice | null>(null);
  const [characteristic, setCharacteristic] = useState<BluetoothRemoteGATTCharacteristic | null>(null);
  const [status, setStatus] = useState("Disconnected");
  const [isConnected, setIsConnected] = useState(false);
  const [connectedDeviceName, setConnectedDeviceName] = useState<string | null>(null);
  const [sensitivity, setSensitivity] = useState(5);
  const [heartbeatCharacteristic, setHeartbeatCharacteristic] = useState<BluetoothRemoteGATTCharacteristic | null>(null);
  const [debugMode, setDebugMode] = useState(false);
  const [debugLogs, setDebugLogs] = useState<string[]>([]);
  const [featuresEnabled, setFeaturesEnabled] = useState(FEATURE_KEY | FEATURE_TEMPO);
  const featuresRef = useRef(FEATURE_KEY | FEATURE_TEMPO);
  const [isListening, setIsListening] = useState(false);
  const isListeningRef = useRef(false);
  const [selectedGenre, setSelectedGenre] = useState(-1);
  const genreRef = useRef(-1);
  const featuresCharacteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const featuresHandlerRef = useRef<((e: Event) => void) | null>(null);
  const deviceRef = useRef<BluetoothDevice | null>(null);
  const characteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const heartbeatCharacteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const debugCharacteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const debugHandlerRef = useRef<((e: Event) => void) | null>(null);
  const logContainerRef = useRef<HTMLDivElement>(null);
  const lastSentRef = useRef<{ newSens: number; newPalette: Record<string, string>; newFeatures: number; newGenre: number } | null>(null);
  const pendingWriteRef = useRef<{ newSens: number; newPalette: Record<string, string>; newFeatures: number; newGenre: number } | null>(null);
  const writeInFlightRef = useRef(false);

  const initialPalette: Record<string, string> = {
    "C Major": "#FF0000", "C Minor": "#8B0000",
    "C# Major": "#FF4500", "C# Minor": "#8B2500",
    "D Major": "#FFA500", "D Minor": "#8B5A00",
    "D# Major": "#FFD700", "D# Minor": "#8B7500",
    "E Major": "#FFFF00", "E Minor": "#8B8B00",
    "F Major": "#ADFF2F", "F Minor": "#556B2F",
    "F# Major": "#00FF00", "F# Minor": "#006400",
    "G Major": "#00FA9A", "G Minor": "#008B45",
    "G# Major": "#00FFFF", "G# Minor": "#008B8B",
    "A Major": "#0000FF", "A Minor": "#00008B",
    "A# Major": "#8A2BE2", "A# Minor": "#4B0082",
    "B Major": "#FF00FF", "B Minor": "#8B008B"
  };
  const [palette, setPalette] = useState(initialPalette);

  useEffect(() => {
    deviceRef.current = device;
  }, [device]);

  useEffect(() => {
    characteristicRef.current = characteristic;
  }, [characteristic]);

  useEffect(() => {
    heartbeatCharacteristicRef.current = heartbeatCharacteristic;
  }, [heartbeatCharacteristic]);

  useEffect(() => {
    featuresRef.current = featuresEnabled;
  }, [featuresEnabled]);

  useEffect(() => {
    genreRef.current = selectedGenre;
  }, [selectedGenre]);

  useEffect(() => {
    const el = logContainerRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [debugLogs]);

  const handleDisconnected = () => {
    const debugChar = debugCharacteristicRef.current;
    const handler = debugHandlerRef.current;
    if (debugChar && handler) {
      debugChar.removeEventListener("characteristicvaluechanged", handler);
      debugChar.stopNotifications().catch(() => {});
    }
    debugCharacteristicRef.current = null;
    debugHandlerRef.current = null;
    const featuresChar = featuresCharacteristicRef.current;
    const featuresHandler = featuresHandlerRef.current;
    if (featuresChar && featuresHandler) {
      featuresChar.removeEventListener("characteristicvaluechanged", featuresHandler);
      featuresChar.stopNotifications().catch(() => {});
    }
    featuresCharacteristicRef.current = null;
    featuresHandlerRef.current = null;
    setFeaturesEnabled(FEATURE_KEY | FEATURE_TEMPO);
    featuresRef.current = FEATURE_KEY | FEATURE_TEMPO;
    setIsListening(false);
    isListeningRef.current = false;
    setSelectedGenre(-1);
    genreRef.current = -1;
    pendingWriteRef.current = null;
    writeInFlightRef.current = false;
    lastSentRef.current = null;
    setDebugMode(false);
    setDevice(null);
    setCharacteristic(null);
    setHeartbeatCharacteristic(null);
    setIsConnected(false);
    setConnectedDeviceName(null);
    setStatus("Watch disconnected");
  };

  useEffect(() => {
    if (!device) {
      return;
    }

    const interval = window.setInterval(() => {
      if (!device.gatt?.connected) {
        handleDisconnected();
      }
    }, 1000);

    return () => window.clearInterval(interval);
  }, [device]);

  const connectToWatch = async () => {
    try {
      setStatus("Requesting Bluetooth Device");
      const selectedDevice = await navigator.bluetooth.requestDevice({
        filters: [{ services: [WEARINGAID_SERVICE_UUID] }],
      });

      // Always reset GATT state. Stale paired devices have connected=false but still fail
      try { selectedDevice.gatt?.disconnect(); } catch {}
      await new Promise<void>(r => setTimeout(r, 200));

      setStatus("Connecting...");
      let server: BluetoothRemoteGATTServer | undefined;
      try {
        server = await selectedDevice.gatt?.connect();
      } catch {
        setStatus("Retrying...");
        await new Promise<void>(r => setTimeout(r, 600));
        server = await selectedDevice.gatt?.connect();
      }

      setStatus("Getting Service");
      const service = await server?.getPrimaryService(WEARINGAID_SERVICE_UUID);

      setStatus("Getting Config Characteristic");
      const configChar = await service?.getCharacteristic(CONFIG_CHARACTERISTIC_UUID);
      const heartbeatChar = await service?.getCharacteristic(HEARTBEAT_CHARACTERISTIC_UUID);
      const debugChar = await service?.getCharacteristic(DEBUG_CHARACTERISTIC_UUID).catch(() => null);
      debugCharacteristicRef.current = debugChar ?? null;

      let gotInitialWatchState = false;
      const featuresChar = await service?.getCharacteristic(FEATURES_CHARACTERISTIC_UUID).catch(() => null);
      if (featuresChar) {
        featuresCharacteristicRef.current = featuresChar;

        const applyFeaturesValue = (value: DataView) => {
          const parts = new TextDecoder().decode(value).split(",");
          const bits = parseInt(parts[0]);
          if (!isNaN(bits)) {
            setFeaturesEnabled(bits & ~FEATURE_LISTENING);
            featuresRef.current = bits & ~FEATURE_LISTENING;
            const listening = (bits & FEATURE_LISTENING) !== 0;
            setIsListening(listening);
            isListeningRef.current = listening;
            const genre = parseInt(parts[1] ?? "-1");
            if (!isNaN(genre) && genre >= 0 && genre <= 4) {
              setSelectedGenre(genre);
              genreRef.current = genre;
            }
            const ts = new Date().toLocaleTimeString();
            const genreTitle = genre >= 0 && genre <= 4 ? GENRE_PROFILES[genre]?.title : "–";
            const featStr = [(bits & FEATURE_KEY ? "Key" : ""), (bits & FEATURE_TEMPO ? "Tempo" : "")].filter(Boolean).join("+") || "none";
            setDebugLogs(prev => [...prev, `[${ts}] WATCH  ${listening ? "▶ listening" : "⏸ paused"} · ${featStr} · ${genreTitle}`].slice(-200));
          }
        };

        const handler = (e: Event) => {
          const value = (e.target as BluetoothRemoteGATTCharacteristic).value;
          if (!value) return;
          applyFeaturesValue(value);
        };
        featuresHandlerRef.current = handler;
        try {
          await featuresChar.startNotifications();
          featuresChar.addEventListener("characteristicvaluechanged", handler);
          // Explicit read guarantees initial state even when push-on-subscribe didn't fire
          const currentValue = await featuresChar.readValue();
          applyFeaturesValue(currentValue);
          gotInitialWatchState = true;
        } catch (e) {
          console.error("Failed to subscribe to features characteristic:", e);
        }
      }

      if (!configChar || !heartbeatChar) {
        setStatus("Connection failed: characteristic not found.");
        selectedDevice.gatt?.disconnect();
        return;
      }

      setIsConnected(true);
      setConnectedDeviceName(selectedDevice.name ?? null);
      setStatus(`Connected to ${selectedDevice.name ?? "Watch"}`);
      const ts = new Date().toLocaleTimeString();
      setDebugLogs(prev => [...prev, `[${ts}] CONN   paired with ${selectedDevice.name ?? "Watch"}`].slice(-200));
      setDevice(selectedDevice);
      setCharacteristic(configChar);
      setHeartbeatCharacteristic(heartbeatChar);
      // The state setters above only reach the refs after the next render; set the
      // refs synchronously so the initial config push below can flush right now.
      deviceRef.current = selectedDevice;
      characteristicRef.current = configChar;
      heartbeatCharacteristicRef.current = heartbeatChar;

      selectedDevice.addEventListener("gattserverdisconnected", handleDisconnected);

      try {
        await heartbeatChar.writeValue(toArrayBuffer(new Uint8Array([1])));
      } catch (error) {
        console.error("Initial heartbeat failed:", error);
        if (isLostGattConnectionError(error)) {
          handleDisconnected();
          return;
        }
      }

      // Push the full config (palette + sensitivity) immediately so the watch's key
      // colors match the companion's from the moment of connection — without this the
      // watch keeps whatever palette it booted with until the user edits a control.
      // Features/genre/listening in the packet echo what was just READ from the watch
      // (refs set in applyFeaturesValue), so this write doesn't clobber watch state.
      // Skipped if the initial state read failed: pushing defaults blind could pause
      // a watch that is currently listening.
      if (gotInitialWatchState) {
        queueConfigPacket(sensitivity, palette);
      }
    } catch (error) {
      console.error(error);
      setStatus(`Connection failed: ${error instanceof Error ? error.message : error}`);
    }
  };

  const flushPendingWrite = async () => {
    if (writeInFlightRef.current) {
      return;
    }

    const pending = pendingWriteRef.current;
    const activeCharacteristic = characteristicRef.current;

    if (!pending || !activeCharacteristic || !deviceRef.current?.gatt?.connected) {
      return;
    }

    pendingWriteRef.current = null;
    writeInFlightRef.current = true;

    try {
      const colorValues = Object.values(pending.newPalette);
      const payload = `${pending.newFeatures},${pending.newSens},${pending.newGenre},${colorValues.join(',')}`;
      const encoded = new TextEncoder().encode(payload);
      await activeCharacteristic.writeValue(toArrayBuffer(encoded));
      const ts = new Date().toLocaleTimeString();
      const prev = lastSentRef.current;
      const changes: string[] = [];
      if (!prev || prev.newFeatures !== pending.newFeatures) {
        changes.push(`listen:${(pending.newFeatures & FEATURE_LISTENING) ? "on" : "off"} key:${(pending.newFeatures & FEATURE_KEY) ? "on" : "off"} tempo:${(pending.newFeatures & FEATURE_TEMPO) ? "on" : "off"}`);
      }
      if (!prev || prev.newSens !== pending.newSens) changes.push(`sens:${pending.newSens}`);
      if (!prev || prev.newGenre !== pending.newGenre) {
        const g = GENRE_PROFILES.find(p => p.code === pending.newGenre);
        changes.push(`genre:${g?.title ?? "–"}`);
      }
      if (prev) {
        Object.entries(pending.newPalette).forEach(([key, val]) => {
          if (prev.newPalette[key] !== val) changes.push(`${key}:${val}`);
        });
      } else {
        changes.push("(initial)");
      }
      lastSentRef.current = { ...pending, newPalette: { ...pending.newPalette } };
      setDebugLogs(prev2 => [...prev2, `[${ts}] SENT   ${changes.join(" · ") || "–"}`].slice(-200));
    } catch (error) {
      console.error("Failed to send packet:", error);
      if (isLostGattConnectionError(error)) {
        handleDisconnected();
      }
    } finally {
      writeInFlightRef.current = false;

      if (pendingWriteRef.current) {
        void flushPendingWrite();
      }
    }
  };

  const sendHeartbeat = async () => {
    if (writeInFlightRef.current) {
      return;
    }

    const activeHeartbeatCharacteristic = heartbeatCharacteristicRef.current;
    if (!activeHeartbeatCharacteristic || !deviceRef.current?.gatt?.connected) {
      return;
    }

    writeInFlightRef.current = true;

    try {
      await activeHeartbeatCharacteristic.writeValue(new Uint8Array([1]));
    } catch (error) {
      console.error("Heartbeat failed:", error);
      handleDisconnected();
    } finally {
      writeInFlightRef.current = false;

      if (pendingWriteRef.current) {
        void flushPendingWrite();
      }
    }
  };

  useEffect(() => {
    if (!device || !heartbeatCharacteristic) {
      return;
    }

    const interval = window.setInterval(() => {
      void sendHeartbeat();
    }, 1500);

    return () => window.clearInterval(interval);
  }, [device, heartbeatCharacteristic]);

  const toggleDebugMode = async () => {
    const debugChar = debugCharacteristicRef.current;
    if (!debugChar) return;

    if (!debugMode) {
      const handler = (e: Event) => {
        const value = (e.target as BluetoothRemoteGATTCharacteristic).value;
        if (!value) return;
        const text = new TextDecoder().decode(value);
        const parts = Object.fromEntries(text.split(",").map(p => p.split("=")));
        const keyIdx = parseInt(parts["k"] ?? "-1");
        const bpm = parts["b"] ?? "?";
        const rms = parts["r"] ?? "?";
        const keyName = keyIdx >= 0 && keyIdx < ENGINE_KEYS.length ? ENGINE_KEYS[keyIdx] : "Silence";
        const ts = new Date().toLocaleTimeString();
        setDebugLogs(prev => [...prev, `[${ts}] AUDIO  ${keyName} · ${bpm} BPM · RMS ${rms}`].slice(-200));
      };
      debugHandlerRef.current = handler;
      try {
        await debugChar.startNotifications();
        debugChar.addEventListener("characteristicvaluechanged", handler);
        setDebugMode(true);
      } catch (err) {
        console.error("Failed to start debug notifications:", err);
      }
    } else {
      const handler = debugHandlerRef.current;
      if (handler) debugChar.removeEventListener("characteristicvaluechanged", handler);
      debugHandlerRef.current = null;
      await debugChar.stopNotifications().catch(() => {});
      setDebugMode(false);
    }
  };

  const queueConfigPacket = (newSens: number, newPalette: Record<string, string>, newFeatures?: number, newGenre?: number) => {
    setSensitivity(newSens);
    setPalette(newPalette);
    if (newFeatures !== undefined) {
      setFeaturesEnabled(newFeatures & ~FEATURE_LISTENING);
      featuresRef.current = newFeatures & ~FEATURE_LISTENING;
    }

    const baseFeatures = (newFeatures ?? featuresRef.current) & ~FEATURE_LISTENING;
    pendingWriteRef.current = {
      newSens,
      newPalette,
      newFeatures: baseFeatures | (isListeningRef.current ? FEATURE_LISTENING : 0),
      newGenre: newGenre ?? genreRef.current,
    };
    void flushPendingWrite();
  };

  const toggleListeningOnCompanion = () => {
    const next = !isListeningRef.current;
    setIsListening(next);
    isListeningRef.current = next;
    pendingWriteRef.current = {
      newSens: sensitivity,
      newPalette: palette,
      newFeatures: featuresRef.current | (next ? FEATURE_LISTENING : 0),
      newGenre: genreRef.current,
    };
    void flushPendingWrite();
  };

  const selectGenreOnCompanion = (code: number) => {
    setSelectedGenre(code);
    genreRef.current = code;
    queueConfigPacket(sensitivity, palette, undefined, code);
  };

  const toggleFeatureOnCompanion = (featureBit: number) => {
    queueConfigPacket(sensitivity, palette, featuresRef.current ^ featureBit);
  };

  const handleColorChange = (keyName: string, newHex: string) => {
    const updatedPalette = { ...palette, [keyName]: newHex };
    queueConfigPacket(sensitivity, updatedPalette);
  };

  return (
    <main className="flex min-h-screen flex-col items-center justify-center p-24 bg-gray-950 text-white">
      <div className="z-10 max-w-5xl w-full items-center justify-between font-mono text-sm flex flex-col gap-8">
        <h1 className="text-4xl font-bold tracking-tight">WearingAid Companion</h1>

        <div className="flex flex-col items-center bg-gray-900 p-8 rounded-xl border border-gray-800 shadow-2xl w-full max-w-md">
          <div className="flex items-center justify-between w-full mb-6">
            <span className="text-gray-400">Status:</span>
            <span className={`font-semibold ${isConnected ? "text-green-400" : "text-yellow-400"}`}>
              {isConnected && connectedDeviceName ? `Connected to ${connectedDeviceName}` : status}
            </span>
          </div>

          {!device ? (
            <button
              onClick={connectToWatch}
              className="w-full bg-blue-600 hover:bg-blue-500 text-white font-bold py-3 px-4 rounded transition-colors"
            >
              Pair with Watch
            </button>
          ) : (
            <div className="w-full flex flex-col gap-4">
              <div className="flex items-center justify-between">
                <span className="text-gray-300 font-semibold">Listening</span>
                <div className="flex rounded overflow-hidden border border-gray-600 text-xs font-semibold">
                  <button
                    onClick={() => { if (!isListening) toggleListeningOnCompanion(); }}
                    className={`px-3 py-1 transition-colors ${isListening ? "bg-blue-600 text-white" : "bg-gray-800 text-gray-400 hover:bg-gray-700"}`}
                  >On</button>
                  <button
                    onClick={() => { if (isListening) toggleListeningOnCompanion(); }}
                    className={`px-3 py-1 transition-colors ${!isListening ? "bg-gray-600 text-white" : "bg-gray-800 text-gray-400 hover:bg-gray-700"}`}
                  >Off</button>
                </div>
              </div>

              <div className="w-full flex flex-col gap-2">
                <span className="text-gray-300 font-semibold">Genre</span>
                <div className="flex flex-col gap-1">
                  {GENRE_PROFILES.map((g) => (
                    <button
                      key={g.code}
                      onClick={() => selectGenreOnCompanion(g.code)}
                      className={`flex items-center justify-between px-3 py-2 rounded border transition-colors text-left ${
                        selectedGenre === g.code
                          ? "bg-blue-600/30 border-blue-500 text-blue-200"
                          : "bg-gray-800 border-gray-700 text-gray-300 hover:bg-gray-700"
                      }`}
                    >
                      <span className="font-medium text-sm">{g.title}</span>
                      <span className="text-xs text-gray-400">{g.subtitle}</span>
                    </button>
                  ))}
                </div>
              </div>

              <label className="flex items-center gap-3 cursor-pointer">
                <input
                  type="checkbox"
                  checked={(featuresEnabled & FEATURE_TEMPO) !== 0}
                  onChange={() => toggleFeatureOnCompanion(FEATURE_TEMPO)}
                  className="w-4 h-4 accent-blue-500"
                />
                <span className="text-gray-300 font-semibold">Tempo Tracking</span>
              </label>
              {(featuresEnabled & FEATURE_TEMPO) !== 0 && (
                <label className="flex flex-col gap-2">
                  <span className="text-gray-300">Vibration Sensitivity: {sensitivity}</span>
                  <input
                    type="range" min="1" max="10" value={sensitivity}
                    onChange={(e) => queueConfigPacket(Number(e.target.value), palette)}
                    className="w-full accent-blue-500"
                  />
                </label>
              )}

              <label className="flex items-center gap-3 cursor-pointer">
                <input
                  type="checkbox"
                  checked={(featuresEnabled & FEATURE_KEY) !== 0}
                  onChange={() => toggleFeatureOnCompanion(FEATURE_KEY)}
                  className="w-4 h-4 accent-blue-500"
                />
                <span className="text-gray-300 font-semibold">Key Detection</span>
              </label>
              {(featuresEnabled & FEATURE_KEY) !== 0 && (
                <div className="w-full flex flex-col gap-4">
                  <span className="text-gray-300 font-semibold border-b border-gray-700 pb-2">
                    Musical Key Palette (24 Channels)
                  </span>
                  <div className="grid grid-cols-2 md:grid-cols-3 gap-3 max-h-96 overflow-y-auto p-2 bg-gray-950 rounded-lg border border-gray-800">
                    {Object.entries(palette).map(([keyName, hexColor]) => (
                      <label key={keyName} className="flex items-center justify-between gap-2 bg-gray-900 p-2 rounded border border-gray-700">
                        <span className="text-gray-300 text-xs font-medium truncate w-full">{keyName}</span>
                        <input
                          type="color" value={hexColor}
                          onChange={(e) => handleColorChange(keyName, e.target.value)}
                          className="w-6 h-6 rounded cursor-pointer bg-transparent border-0 p-0 flex-shrink-0"
                        />
                      </label>
                    ))}
                  </div>
                </div>
              )}

              <button
                onClick={() => void toggleDebugMode()}
                className={`w-full mt-2 font-bold py-2 px-4 rounded border transition-colors ${
                  debugMode
                    ? "bg-yellow-900/50 hover:bg-yellow-900 text-yellow-200 border-yellow-700"
                    : "bg-gray-800 hover:bg-gray-700 text-gray-300 border-gray-600"
                }`}
              >
                {debugMode ? "Audio Debug: On" : "Audio Debug: Off"}
              </button>

              <div className="w-full flex flex-col gap-2 mt-2">
                <div className="flex items-center justify-between border-b border-gray-700 pb-1">
                  <span className="text-gray-400 text-xs font-semibold">Activity Log</span>
                  <button onClick={() => setDebugLogs([])} className="text-gray-600 hover:text-gray-400 text-xs transition-colors">Clear</button>
                </div>
                <div
                  ref={logContainerRef}
                  className="bg-gray-950 rounded border border-gray-800 p-2 h-48 overflow-y-auto font-mono text-xs text-green-400 flex flex-col gap-0.5"
                >
                  {debugLogs.length === 0 ? (
                    <span className="text-gray-600">No activity yet…</span>
                  ) : (
                    debugLogs.map((line, i) => <span key={i}>{line}</span>)
                  )}
                </div>
              </div>

              <button
                onClick={() => device.gatt?.disconnect()}
                className="w-full mt-4 bg-red-900/50 hover:bg-red-900 text-red-200 font-bold py-2 px-4 rounded border border-red-800 transition-colors"
              >
                Disconnect
              </button>
            </div>
          )}
        </div>
      </div>
    </main>
  );
}