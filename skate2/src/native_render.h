// Native renderer entry point (see native_render.cpp).

#pragma once

// Registers the guest-output renderer with the SDK. Call once at startup.
void InstallSkate2NativeRenderer();

// Whether the last completed frame drew the 3D world (native_probe.cpp).
bool Skate2WorldSceneActive();
