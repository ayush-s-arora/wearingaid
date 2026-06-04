/// <reference types="web-bluetooth" />
"use client";

import { useState } from "react";

// The custom 16-bit UUID for the WearingAid service. 
// We will code the WearOS app to broadcast this exact ID.
const WEARINGAID_SERVICE_UUID = "696afb11-bed7-42cc-b178-dd49bac6c8ef";
const CONFIG_CHARACTERISTIC_UUID = "692580ea-d39f-49f8-bb81-ae799d99de8d";

export default function Home() {
  const [device, setDevice] = useState<BluetoothDevice | null>(null);
  const [deviceName, setDeviceName] = useState<string>("Unknown Device");
  const [characteristic, setCharacteristic] = useState<BluetoothRemoteGATTCharacteristic | null>(null);
  const [status, setStatus] = useState("Disconnected");
  const [sensitivity, setSensitivity] = useState(5);

  const connectToWatch = async () => {
    try {
      setStatus("Requesting Bluetooth Device...");
      // This triggers the native browser pairing popup
      const selectedDevice = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: "WearingAid" }],
        optionalServices: [WEARINGAID_SERVICE_UUID],
      });

      setStatus("Connecting to GATT Server...");
      const server = await selectedDevice.gatt?.connect();

      setStatus("Getting Service...");
      const service = await server?.getPrimaryService(WEARINGAID_SERVICE_UUID);

      setStatus("Getting Characteristic...");
      const char = await service?.getCharacteristic(CONFIG_CHARACTERISTIC_UUID);

      setDevice(selectedDevice);
      setDeviceName(selectedDevice.name || "Unknown Device");
      setCharacteristic(char || null);
      setStatus(`Connected: ${selectedDevice.name}`);

      // Handle disconnects gracefully
      selectedDevice.addEventListener("gattserverdisconnected", () => {
        setDevice(null);
        setDeviceName("Unknown Device");
        setCharacteristic(null);
        setStatus("Disconnected");
      });

    } catch (error) {
      console.error(error);
      setStatus(`Connection failed: ${error}`);
    }
  };

  const sendConfigPacket = async (newSensitivity: number) => {
    setSensitivity(newSensitivity);
    if (!characteristic) return;

    try {
      // Encode the JSON payload into a byte array
      const payload = JSON.stringify({ mode: "tempo", sensitivity: newSensitivity });
      const encoder = new TextEncoder();
      const data = encoder.encode(payload);

      await characteristic.writeValue(data);
      console.log("Packet sent:", payload);
    } catch (error) {
      console.error("Failed to send packet:", error);
    }
  };

  return (
    <main className="flex min-h-screen flex-col items-center justify-center p-24 bg-gray-950 text-white">
      <div className="z-10 max-w-5xl w-full items-center justify-between font-mono text-sm flex flex-col gap-8">
        
        <h1 className="text-4xl font-bold tracking-tight">WearingAid Companion</h1>
        
        <div className="flex flex-col items-center bg-gray-900 p-8 rounded-xl border border-gray-800 shadow-2xl w-full max-w-md">
          <div className="flex items-center justify-between w-full mb-6">
            <span className="text-gray-400">Status:</span>
            <span className={`font-semibold ${status === "Connected and Ready" ? "text-green-400" : "text-yellow-400"}`}>
              {status}
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
                  type="range"
                  min="1"
                  max="10"
                  value={sensitivity}
                  onChange={(e) => sendConfigPacket(Number(e.target.value))}
                  className="w-full accent-blue-500"
                />
              </label>
              
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