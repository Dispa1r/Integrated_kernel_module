---
name: deploy-android-toolkit
description: Deploy lsdriver kernel module + Zygisk rfrida module to Android device and verify
---

# Deploy Android RE Toolkit

Deploy and verify both the **lsdriver kernel module** and **Zygisk rfrida injection module**
on a connected Android device.

## Prerequisites

```bash
adb devices         # must show one device
adb shell su -c id  # must show uid=0(root)
```

## Step 1: Load kernel module (lsdriver.ko)

```bash
# Push and load
adb push build_output/lsdriver_stripped.ko /data/local/tmp/lsdriver.ko
adb shell su -c "dmesg -c > /dev/null; insmod /data/local/tmp/lsdriver.ko 2>&1; echo RC=\$?"

# Verify: should show 2+ ext4-rsv-conver threads (hidden from ps but visible to kernel)
sleep 1
adb shell su -c "dmesg | grep -iE 'lsdriver|wxshadow'"
```

### Troubleshooting

If insmod fails:

```bash
# Check vermagic mismatch
adb shell su -c "modinfo /data/local/tmp/lsdriver.ko | grep vermagic"
adb shell uname -r

# If mismatch, patch vermagic in the binary before pushing:
python3 -c "
d=bytearray(open('build_output/lsdriver.ko','rb').read())
# Find current vermagic
import re; m=re.search(rb'vermagic=[^\x00]+', d)
if m:
    print(f'Current: {m.group().decode()}')
    print(f'Target:  {adb_output_of_uname_r}')
    # Patch needed: adjust search/replace
"
```

## Step 2: Deploy Zygisk rfrida module

```bash
# Create module directory structure
adb shell su -c "mkdir -p /data/adb/modules/rfrida_zygisk/zygisk"

# Push module files
adb push rfrida_zygisk/build/librfrida_zygisk.so /data/local/tmp/
adb shell su -c "cp /data/local/tmp/librfrida_zygisk.so /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so"

# Push agent.so (the .so that gets injected into target processes)
adb push libagent.so /data/local/tmp/
adb shell su -c "cp /data/local/tmp/libagent.so /data/adb/modules/rfrida_zygisk/agent.so"

# Create module.prop
adb shell su -c "cat > /data/adb/modules/rfrida_zygisk/module.prop << EOF
id=rfrida_zygisk
name=rfrida Zygisk Injector
version=v1.0
versionCode=1
author=rfrida
description=Stealth agent injection via custom ELF linker
EOF"

# Fix SELinux context (critical! Zygisk needs system_file context)
adb shell su -c "
chcon -R u:object_r:system_file:s0 /data/adb/modules/rfrida_zygisk/
chcon u:object_r:system_lib_file:s0 /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so
"

# Reboot to load Zygisk module
adb reboot
adb wait-for-device
sleep 20
```

## Step 3: Verify kernel module

```bash
# Check kernel threads
adb shell su -c "ps -A | grep -c ext4-rsv-conver"
# 0 is OK — threads are hidden via hide_task_install

# Check dmesg
adb shell su -c "dmesg | grep -iE 'lsdriver|wxshadow'"

# Start HTTP bridge and test
adb forward tcp:19494 tcp:19494
adb shell su -c "echo 4 | /data/local/tmp/LS_KTool &"
sleep 5

# Test kernel ping
python3 -c "
import http.client, json
c = http.client.HTTPConnection('127.0.0.1', 19494, timeout=5)
c.request('POST', '/api/rpc', json.dumps({'operation':'bridge.ping','params':{}}))
r = json.loads(c.read())
print('Kernel bridge:', 'OK' if r.get('ok') else 'FAILED')
c.close()
"

# Test WXShadow
python3 -c "
import http.client, json
c = http.client.HTTPConnection('127.0.0.1', 19494, timeout=5)
c.request('POST', '/api/rpc', json.dumps({'operation':'target.attach','params':{'package_name':'com.android.systemui'}}))
r = json.loads(c.read())
pid = r.get('data',{}).get('pid')
c.close()
if pid:
    c = http.client.HTTPConnection('127.0.0.1', 19494, timeout=5)
    c.request('POST', '/api/rpc', json.dumps({'operation':'wxshadow.get_state','params':{}}))
    r = json.loads(c.read())
    print('WXShadow:', 'OK' if r.get('ok') else 'FAILED', r.get('data',{}))
    c.close()
"
```

## Step 4: Verify Zygisk module

```bash
# Check module loaded into processes
adb shell "logcat -d -s rfrida-zygisk:* 2>/dev/null | tail -10"

# Should see:
#   I rfrida-zygisk: onLoad pid=XXXX
#   I rfrida-zygisk: preAppSpecialize: nice_name=com.android.systemui
#   I rfrida-zygisk: Target check: com.android.systemui -> 1
#   I rfrida-zygisk: Agent loaded at 0x...
#   I rfrida-zygisk: Injection complete!

# Kill and restart systemui to trigger injection
adb shell su -c "killall com.android.systemui"
sleep 12
adb shell "logcat -d -s rfrida-zygisk:* 2>/dev/null | grep -iE 'inject|agent|loaded|entry'"

# Check proc/maps for agent (should NOT appear — custom linker uses anonymous mmap)
adb shell su -c "cat /proc/\$(pidof com.android.systemui)/maps | grep -i agent"
# Empty output = SUCCESS (agent is invisible!)
```

## Step 5: Test with MCP tools

```bash
# Start HTTP bridge if needed
adb forward tcp:19494 tcp:19494

python3 << 'PYEOF'
import sys; sys.path.insert(0, 'rfrida_mcp'); from server import *

# Test kernel operations
r = kernel_ping()
print(f'Kernel: {"OK" if r["ok"] else "DOWN"}')

# Test WXShadow
r2 = kernel_wxshadow_state()
print(f'WXShadow: {r2.get("data",{}).get("total_pages",0)} pages')

# Test injection
r3 = inject_list_processes(filter_name="systemui")
print(f'Processes found: {r3["count"]}')
PYEOF
```

## Quick Health Check (all-in-one)

```bash
echo "=== Kernel Module ==="
adb shell su -c "dmesg | grep -c 'lsdriver'" | xargs -I{} echo "  lsdriver messages: {}"
adb shell su -c "ps -A | grep -c ext4-rsv-conver" | xargs -I{} echo "  kernel threads: {} (0=hidden OK)"

echo "=== Zygisk Module ==="
adb shell su -c "ls -la /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so" | grep -q "arm64-v8a" && echo "  module.so: OK" || echo "  module.so: MISSING"
adb shell su -c "ls -laZ /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so" | grep -q "system_lib_file" && echo "  SELinux: OK" || echo "  SELinux: WRONG"

echo "=== HTTP Bridge ==="
adb shell su -c "ss -tlnp | grep -q 19494" && echo "  bridge: LISTENING" || echo "  bridge: DOWN"

echo "=== Full Stack: Kernel + Zygisk = OK ==="
```
