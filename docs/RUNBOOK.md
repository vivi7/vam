# Camswap Runbook

## Recovery Procedures

### Module Not Loading
```bash
# Stop camswap
su -c '/data/local/tmp/payload_stop'

# Restart cameraserver
stop cameraserver
start cameraserver

# If still not working
setprop ctl.restart cameraserver
```

### System Unstable
```bash
# Boot to bootloader
vol+ + power -> bootloader

# Boot without module
fastboot boot <boot.img without module>
```

### Camera Not Working
```bash
# Stop camswap
su -c '/data/local/tmp/payload_stop'

# Restart cameraserver
stop cameraserver
start cameraserver
```

## Testing Procedures

### Gate A (Fase 2)
1. Run preflight.sh
2. Install module
3. Test each app for 10 minutes:
   - WhatsApp (video call)
   - Browser (getUserMedia)
   - Camera app
4. Monitor for crashes, deadlocks, buffer leaks

### Gate B (Fase 3)
1. Run 20 minutes with PSNR monitoring
2. Verify PSNR stability
3. Check for any crashes

### Gate C (Fase 5+)
1. Run 30 minutes continuous
2. Monitor FPS (must stay above 15)
3. Monitor temperature (must stay below 42°C)
4. Check for crashes

## Troubleshooting

### High FPS Drop
- Check thermal state
- Verify ring buffer is not full
- Check if AI model is loading correctly

### Camera Crashes
- Check SELinux denials
- Verify hook symbol resolution
- Check memory usage

### Thermal Issues
- Verify thermal ladder is working
- Check if passthrough mode activates at 44°C
- Consider reducing ROI size

## Log Locations

- `/data/local/tmp/preflight.log` - Preflight check log
- `/data/local/tmp/camswap.log` - Camswap daemon log
- `adb logcat` - System logs

## Common Issues

### Issue: Module not loading
**Solution:** Check module.prop, verify Magisk version, check sepolicy rules

### Issue: Camera crashes after hook
**Solution:** Verify symbol resolution, check SELinux denials, verify buffer layout

### Issue: High temperature
**Solution:** Verify thermal ladder, check if passthrough activates, reduce FPS target

### Issue: FPS below floor
**Solution:** Check ring buffer, verify AI model performance, check thermal state
