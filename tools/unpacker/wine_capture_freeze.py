import subprocess, os, time, signal
env=dict(os.environ)
p=subprocess.Popen(['wine','MightyQuest.exe'], cwd='/tmp/run',
                   stdout=open('/tmp/run/cap.log','wb'), stderr=subprocess.STDOUT, env=env)
def pids():
    return [int(x) for x in subprocess.run(['pgrep','-f','MightyQuest.exe'],capture_output=True,text=True).stdout.split()]
time.sleep(1.2)
frozen=None
for _ in range(300):
    for pid in pids():
        try:
            mp=open(f'/proc/{pid}/maps').read()
            if '00400000' not in mp: continue
            with open(f'/proc/{pid}/mem','rb',0) as m:
                m.seek(0x401000); txt=m.read(0x4000)
            nz=sum(1 for b in txt if b)
            if nz>10000:
                # FREEZE everything
                for q in pids():
                    try: os.kill(q, signal.SIGSTOP)
                    except Exception: pass
                frozen=pid
                break
        except Exception: continue
    if frozen: break
    if p.poll() is not None and not pids(): break
    time.sleep(0.05)
if not frozen:
    print("not frozen"); 
    try: p.kill()
    except: pass
    raise SystemExit
print("FROZEN pid", frozen)
# dump maps and image
open('/tmp/cap_maps.txt','w').write(open(f'/proc/{frozen}/maps').read())
with open(f'/proc/{frozen}/mem','rb',0) as m:
    m.seek(0x400000); img=m.read(0x19c7000)
open('/tmp/cap_image.bin','wb').write(img)
print("saved maps + image; image size", len(img))
# show windows-dll-looking mappings
for line in open('/tmp/cap_maps.txt'):
    l=line.lower()
    if ('.dll' in l or 'system32' in l) and ('r-xp' in l or 'rwxp' in l):
        print("  ", line.strip()[:120])
# leave stopped; kill after
for q in pids():
    try: os.kill(q, signal.SIGKILL)
    except: pass
