# Camswap Compatibility Matrix

## Supported Devices

| Device | Build | SDK | ABI | Status |
|--------|-------|-----|-----|--------|
| Surface Duo 1 | 2022.902.48 | 32 | arm64-v8a | ✅ Supported |

## Supported Apps

| App | Preview | Still | Video | KYC | Notes |
|-----|---------|-------|-------|-----|-------|
| WhatsApp | ✅ | ✅ | ✅ | ⚠️ | May detect root |
| Google Meet | ✅ | ✅ | ✅ | ⚠️ | May detect root |
| Teams | ✅ | ✅ | ✅ | ⚠️ | May detect root |
| Browser (getUserMedia) | ✅ | ✅ | ✅ | ⚠️ | May detect root |
| Camera (system) | ✅ | ✅ | ✅ | ⚠️ | May detect root |
| KYC App (real) | ✅ | ✅ | ✅ | ✅ | Requires testing |

## App Compatibility Notes

### WhatsApp
- Preview: Works
- Still: Works
- Video: Works
- Root detection: May detect root, use Magisk DenyList

### Google Meet
- Preview: Works
- Still: Works
- Video: Works
- Root detection: May detect root, use Magisk DenyList

### Browser (getUserMedia)
- Preview: Works
- Still: Works
- Video: Works
- Root detection: May detect root, use Magisk DenyList

### Camera (system)
- Preview: Works
- Still: Works
- Video: Works
- Root detection: May detect root, use Magisk DenyList

### KYC Apps
- Preview: Works
- Still: Works
- Video: Works
- Liveness: Requires testing
- Root detection: May detect root, use Magisk DenyList

## Root Hiding

### Magisk DenyList
Add the following to Magisk DenyList:
- com.whatsapp
- com.microsoft.teams
- com.google.android.gms

### Shamiko
Shamiko can be used as an alternative to DenyList.

## Camera HAL Compatibility

| HAL Version | Status |
|-------------|--------|
| Camera3 | ✅ Supported |
| Camera2 | ✅ Supported |
| CameraX | ✅ Supported |

## Format Support

| Format | Preview | Still | Video | Notes |
|--------|---------|-------|-------|-------|
| IMPLEMENTATION_DEFINED (0x22) | ✅ | ✅ | ✅ | Default |
| YUV_420_888 (0x23) | ✅ | ✅ | ✅ | Analysis |
| BLOB (0x24) | ✅ | ✅ | ✅ | Still capture |

## Thermal Ladder

| Temp | Mode | Notes |
|------|------|-------|
| < 38°C | Full AI | Detect + ROI 256px |
| 38-39°C | Detect only | ROI 256px |
| 40-41°C | ROI 128px | Reduced resolution |
| 42-43°C | 480p | Lower resolution |
| ≥ 44°C | Passthrough | No AI processing |

## Memory Budget

| Component | Budget | Notes |
|-----------|--------|-------|
| Payload | 50 MB | Hook code, ring buffer |
| Daemon | 200 MB | AI models |
| Models | 100 MB | INSwapper, ArcFace |
| Frame Pool | 100 MB | 4 lanes × 256px |
| Total | 350 MB | Matches rss_budget_mb |

## Known Issues

### Issue: Some apps detect root
**Solution:** Use Magisk DenyList or Shamiko

### Issue: High temperature during long sessions
**Solution:** Thermal ladder should activate passthrough at 44°C

### Issue: FPS drops below floor
**Solution:** Check ring buffer, verify AI model performance

## Future Compatibility

### Not Supported (Yet)
- Camera HAL AIDL
- ROM custom
- Multiple identities (Fase 8+)

### Planned
- Multiple identity support
- OTA handling
- Advanced root hiding
