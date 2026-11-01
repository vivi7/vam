#!/usr/bin/env bash
# Setup repo camswap. Esegui dalla cartella del progetto.
set -e

mkdir -p refs devices tools docs app daemon payload module

# git clone --depth 1 https://github.com/aimardcr/ReCam refs/ReCam
# git clone --depth 1 https://github.com/MJx0/AndKittyInjector refs/AndKittyInjector
# git clone --depth 1 https://github.com/Parasaran-Python/android-face-fusion refs/android-face-fusion
# git clone --depth 1 https://github.com/Xposed-Modules-Repo/com.hazbu.xcam refs/xcam
# git clone --depth 1 https://github.com/Sabbir2k15/ScopeCam refs/ScopeCam
# git clone --depth 1 https://github.com/theGoodB0rg/camera_hook refs/camera_hook
# git clone --depth 1 https://github.com/Atomos-X/Camera2Magic refs/Camera2Magic
# git clone --depth 1 https://github.com/deepinsight/inswapper-512-live refs/inswapper-512-live
# git clone --depth 1 https://github.com/FeiGeChuanShu/ncnn_Android_LivePortrait refs/LivePortrait-ncnn
# git clone --depth 1 https://github.com/jsuppe/aosp-virtual-camera refs/aosp-virtual-camera

# binario cameraserver della build installata: fonte dei simboli reali
mkdir -p refs/bin
adb shell su -c 'cp /system/bin/cameraserver /data/local/tmp/'
adb pull /data/local/tmp/cameraserver refs/bin/
adb shell su -c 'rm /data/local/tmp/cameraserver'

# snapshot ambiente
{
  echo "sdk=$(adb shell getprop ro.build.version.sdk | tr -d '\r')"
  echo "incremental=$(adb shell getprop ro.build.version.incremental | tr -d '\r')"
  echo "device=$(adb shell getprop ro.product.device | tr -d '\r')"
  echo "abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
} > docs/env-snapshot.txt

cat > devices/surfaceduo.env <<'EOF'
device=surfaceduo
soc=sd855
gpu=adreno640
ram_gb=6
sdk_allow=32
build_allow=2022.902.48
lens_policy=all
accel_backend=gpu_gl
fps_target=20
fps_floor=15
rss_budget_mb=350
thermal_ladder=38:detect_rada,40:roi_128,42:480p,44:passthrough
EOF

printf 'refs/\ndocs/reports/\nbuild/\n*.o\n*.so\n' > .gitignore

echo "OK. Ora lancia Claude Code e incolla PROMPT.md come primo messaggio."