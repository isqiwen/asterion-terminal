import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { configureApi, request } from "./client";
import {
  nativeDesktop,
  startDesktop,
  type DesktopSession,
} from "../deployment/desktop";
export function useConnection(start = true) {
  const [phase, setPhase] = useState(0);
  const [token, setToken] = useState("");
  const [connected, setConnected] = useState(false);
  const [starting, setStarting] = useState(nativeDesktop);
  const [error, setError] = useState("");
  const [directory, setDirectory] = useState("");
  async function connect(boot = start) {
    setStarting(true);
    setError("");
    setPhase(0);
    try {
      if (boot) {
        await invoke<DesktopSession | null>("desktop_info");
        setPhase(1);
      }
      const session = await (boot
        ? startDesktop()
        : invoke<DesktopSession | null>("desktop_info"));
      if (session) {
        configureApi(session.api_url);
        setToken(session.token);
        setDirectory(session.data_directory);
        setPhase(2);
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setStarting(false);
    }
  }
  useEffect(() => {
    const reconnect = () => {
      if (nativeDesktop) void connect(true);
    };
    window.addEventListener("asterion:reconnect", reconnect);
    return () => window.removeEventListener("asterion:reconnect", reconnect);
  }, []);
  useEffect(() => {
    if (nativeDesktop) void connect();
  }, []);
  useEffect(() => {
    if (!token) {
      setConnected(false);
      return;
    }
    let active = true;
    const refresh = async () => {
      try {
        await request("/health", token);
        if (active) {
          setConnected(true);
          setPhase(3);
          setError("");
        }
      } catch (e) {
        if (active) {
          setConnected(false);
          setError(String(e));
        }
      }
    };
    void refresh();
    const timer = setInterval(refresh, 2500);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [token]);
  return {
    phase,
    token,
    setToken,
    connected,
    starting,
    error,
    directory,
    connect,
  };
}
