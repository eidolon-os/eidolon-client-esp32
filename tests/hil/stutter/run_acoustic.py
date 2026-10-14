"""Bounded acoustic HIL: real BOX session, no synthetic playback ACKs.
Run only after exclusive device access is established. Restores no firmware itself.
"""
import argparse,json,re,subprocess,time
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--request',required=True);p.add_argument('--out',required=True);p.add_argument('--rounds',type=int,default=3);p.add_argument('--seconds',type=int,default=480);p.add_argument('--port',default='/dev/cu.usbmodem1101');a=p.parse_args()
d=Path(a.out);d.mkdir(parents=True,exist_ok=True)
request=Path(a.request).resolve()
if not request.is_file():p.error('--request must point to an existing audio file')
if a.rounds < 1 or a.seconds < 1:p.error('rounds and seconds must be positive')
started=time.monotonic();deadline=started+a.seconds;accepted=False;last_audible=started;idle=False;sent=0;last_sent=None;player=None;round_audible=False;failed=False
with (d/'events.jsonl').open('w',buffering=1) as ev,(d/'box3.jsonl').open('w',buffering=1) as log:
 def event(kind,**kw):
  row=dict(unix=time.time(),elapsed=round(time.monotonic()-started,3),event=kind,**kw);ev.write(json.dumps(row,ensure_ascii=False)+'\n');print(json.dumps(row,ensure_ascii=False),flush=True)
 s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.2;s.dtr=False;s.rts=False;s.open()
 event('capture_started',rounds=a.rounds)
 try:
  while time.monotonic()<deadline and not (d/'stop').exists():
   now=time.monotonic();line=s.readline().decode('utf-8',errors='replace').strip()
   if line:
    log.write(json.dumps({'unix':time.time(),'line':line},ensure_ascii=False)+'\n')
    if 'reason=session_started' in line:
     accepted=True;event('real_session_accepted')
    if 'Audio state' in line:
     idle='playback=idle' in line
     m=re.search(r'playback_rms=([0-9.]+)',line)
     if m and float(m[1])>.003:
      last_audible=now
      if last_sent is not None and now-last_sent>8:round_audible=True
    if any(x in line for x in ['HIL_HEAP','free sram','rx_diag']):
     pass # Full timestamped telemetry retained in box3.jsonl.
   if last_sent is None and not accepted and now-started>60:
    event('failed',reason='no real session accepted within 60 seconds');failed=True;break
   if last_sent is not None and now-last_sent>45 and not round_audible:
    event('failed',reason='no sustained audible reply after acoustic request',round=sent);failed=True;break
   if player is not None and player.poll() is None:continue
   ready=accepted and idle and now-last_audible>8
   if last_sent is not None:ready=ready and round_audible and now-last_sent>100
   if ready:
    if sent>=a.rounds:
     event('finished_playback_rounds',rounds=sent,note='Verify ASR and source duration from Host logs before declaring valid.');break
    sent+=1;last_sent=now;round_audible=False
    event('acoustic_request',round=sent,clip=str(request))
    player=subprocess.Popen(['/usr/bin/afplay',str(request)])
  else:
   failed=True
   event('bounded_stop',reason='deadline or stop file')
 finally:
  if player is not None and player.poll() is None:player.terminate();player.wait(timeout=3)
  s.close();event('capture_closed',requests=sent,failed=failed)

raise SystemExit(1 if failed else 0)
