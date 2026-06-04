/// <reference types="web-bluetooth" />
"use client";

import { useEffect, useRef, useState } from "react";

const WEARINGAID_SERVICE_UUID = "696afb11-bed7-42cc-b178-dd49bac6c8ef";
const CONFIG_CHARACTERISTIC_UUID = "692580ea-d39f-49f8-bb81-ae799d99de8d";
const HEARTBEAT_CHARACTERISTIC_UUID = "3f2b0ed6-6df7-4f1a-8d77-fc7f2f76d211";

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
  const deviceRef = useRef<BluetoothDevice | null>(null);
  const characteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const heartbeatCharacteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);
  const pendingWriteRef = useRef<{ newSens: number; newPalette: Record<string, string> } | null>(null);
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

  const handleDisconnected = () => {
    pendingWriteRef.current = null;
    writeInFlightRef.current = false;
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
        filters: [{ namePrefix: "WearingAid" }],
        optionalServices: [WEARINGAID_SERVICE_UUID],
      });

      setStatus("Connecting to GATT Server");
      const server = await selectedDevice.gatt?.connect();

      setStatus("Getting Service");
      const service = await server?.getPrimaryService(WEARINGAID_SERVICE_UUID);

      setStatus("Getting Config Characteristic");
      const configChar = await service?.getCharacteristic(CONFIG_CHARACTERISTIC_UUID);
      const heartbeatChar = await service?.getCharacteristic(HEARTBEAT_CHARACTERISTIC_UUID);

      if (!configChar || !heartbeatChar) {
        setStatus("Connection failed: characteristic not found.");
        selectedDevice.gatt?.disconnect();
        return;
      }

      setIsConnected(true);
      setConnectedDeviceName(selectedDevice.name ?? null);
      setStatus(`Connected to ${selectedDevice.name ?? "Watch"}`);
      setDevice(selectedDevice);
      setCharacteristic(configChar);
      setHeartbeatCharacteristic(heartbeatChar);

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
      const payload = `${pending.newSens},${colorValues.join(',')}`;
      const encoded = new TextEncoder().encode(payload);
      await activeCharacteristic.writeValue(toArrayBuffer(encoded));
      console.log("Config packet sent");
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

  useEffect(() => {
    if (!device || !heartbeatCharacteristic) {
      return;
    }

    const interval = window.setInterval(() => {
      void sendHeartbeat();
    }, 1500);

    return () => window.clearInterval(interval);
  }, [device, heartbeatCharacteristic]);

  const queueConfigPacket = (newSens: number, newPalette: Record<string, string>) => {
    setSensitivity(newSens);
    setPalette(newPalette);

    pendingWriteRef.current = { newSens, newPalette };
    void flushPendingWrite();
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
              <label className="flex flex-col gap-2">
                <span className="text-gray-300">Vibration Sensitivity: {sensitivity}</span>
                <input
                  type="range" min="1" max="10" value={sensitivity}
                  onChange={(e) => queueConfigPacket(Number(e.target.value), palette)}
                  className="w-full accent-blue-500"
                />
              </label>

              <div className="w-full flex flex-col gap-4 mt-6">
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