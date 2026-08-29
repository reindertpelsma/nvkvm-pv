# KDE System Settings wedges on the Wayland GL path — OPEN

Found 2026-08-29 immediately after the display fix, on the same guest.
Separate bug from the truncated-install one; the desktop itself is healthy.

## Symptom

`systemsettings` starts, never shows a window, and pins ~98% CPU indefinitely.
Repeated clicks stack up more stuck processes. Nothing is logged and it never
crashes, so `coredumpctl` is empty — it hangs rather than fails.

## Where it hangs

`eu-stack` on the spinning main thread, sampled twice three seconds apart with
an identical stack (so it is wedged, not merely slow):

```
#2..#15  libnvidia-eglcore.so.580.173.02      <- spinning here
#16      QtWaylandClient::QWaylandGLContext::swapBuffers(QPlatformSurface*)
#18      QRhi::endFrame(QRhiSwapChain*, ...)
#20      QPlatformBackingStore::rhiFlush(...)
#21..#23 QWidgetRepaintManager::paintAndFlush()
#34      QCoreApplication::exec()
```

The innermost frame is `clock_gettime` under NVIDIA's EGL — a busy wait, not a
blocking one. Note the frames are in **libnvidia-eglcore**, not Mesa: Mesa and
libgallium are mapped, but only because GLVND probes every vendor.

## Isolation

| run | CPU | window |
|---|---|---|
| Wayland, default (GL) | 98% | none |
| Wayland, `QT_QUICK_BACKEND=software QSG_RHI_BACKEND=software` | 0.1% | opens |
| XWayland, `QT_QPA_PLATFORM=xcb` | 0.1% | opens |
| konsole on Wayland (GL) | 0.2% | opens |

So it is not Wayland generally and not Qt generally. What is specific to
systemsettings is the QQuickWidget/RHI composition path — QML KCMs hosted in a
QWidget window, flushed through `rhiFlush` with a texture list. konsole never
takes that path.

## Workaround

```bash
QT_QUICK_BACKEND=software QSG_RHI_BACKEND=software systemsettings
# or
QT_QPA_PLATFORM=xcb systemsettings
```

## Caveat before acting on this

Every launch above was made with `setpriv --reuid=deck`, which left `HOME=/root`
— visible as a "Configuration file /root/.config/systemsettingsrc not writable"
dialog. A wrong HOME does not plausibly cause a busy wait inside EGL
swapBuffers, and the GL/software split is consistent, but the run should be
repeated as the real `deck` user launching from the desktop before this is
treated as fully characterised.

Also unknown: whether this predates the display fix or is specific to this slot.
Worth checking against a guest on the working image.
