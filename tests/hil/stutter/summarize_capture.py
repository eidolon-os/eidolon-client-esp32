"""Summarize HIL telemetry without equating timestamp gaps to packet loss."""
import argparse,datetime,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('capture');p.add_argument('--out',required=True);a=p.parse_args()
heap=[];largest=[];gaps=[];errors=[];builds=[];hosts=[];speaking=0
for raw in Path(a.capture).read_text().splitlines():
 try:r=json.loads(raw)
 except ValueError:continue
 l=r.get('line','');t=r.get('unix')
 m=re.search(r'free sram: (\d+) minimal sram: (\d+)',l)
 if m:heap.append({'unix':t,'free':int(m[1]),'low_watermark':int(m[2])})
 m=re.search(r'HIL_HEAP largest_internal=(\d+) free_psram=(\d+) largest_psram=(\d+)',l)
 if m:largest.append({'unix':t,'largest_internal':int(m[1]),'free_psram':int(m[2]),'largest_psram':int(m[3])})
 m=re.search(r'\[rx_diag\] (in|out) frames=(\d+) gap>40ms=(\d+) gap>100ms=(\d+) max_gap=(\d+)ms',l)
 if m:gaps.append({'unix':t,'direction':m[1],'frames':int(m[2]),'gaps_over_100ms':int(m[4]),'max_gap_ms':int(m[5])})
 if re.search(r'alloc.*fail|out of memory|CORRUPT|Guru Meditation|DUPLICATE_IDENTITY',l,re.I):errors.append({'unix':t,'line':l})
 if 'git=' in l and 'sdk=' in l:builds.append(l)
 if 'Connecting room=' in l and 'server=' in l:hosts.append(l)
 m=re.search(r'playback_rms=([0-9.]+)',l)
 if m and float(m[1])>.003:speaking+=1
report={'capture':str(Path(a.capture).resolve()),'builds':builds,'connections':hosts,'heap_samples':len(heap),'heap_first':heap[0] if heap else None,'heap_last':heap[-1] if heap else None,'minimum_observed_internal_free':min((x['free'] for x in heap),default=None),'minimum_internal_largest_block':min((x['largest_internal'] for x in largest),default=None),'nonzero_playback_observations':speaking,'audio_gap_windows':gaps,'errors':errors,'heap_timeline':heap,'largest_block_timeline':largest,'interpretation_limits':['No pass/fail for audible stutter from these metrics alone.','RX gap counts include DTX and normal source silence.','Heap low watermark alone does not establish allocation failure.','Host ASR and speech lifecycle must be correlated separately.']}
Path(a.out).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['heap_timeline','largest_block_timeline','audio_gap_windows','builds','connections','errors']},ensure_ascii=False))
