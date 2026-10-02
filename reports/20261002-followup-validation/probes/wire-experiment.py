import socket,struct,time,json
IPS={'192.168.3.194','192.168.3.211','192.168.3.132'}
streams={}
def emit(d):print(json.dumps(d),flush=True)
def var(b,p):
 v=0
 for i in range(10):
  x=b[p];p+=1;v|=(x&127)<<(i*7)
  if x<128:return v,p
 raise ValueError('varint')
def wsmeta(b,d):
 try:
  tag,p=var(b,0);d={'protobuf_field':tag>>3}
  if tag&7==2 and ((d["dp"]==7880 and tag>>3==16) or (d["sp"]==7880 and tag>>3==20)):
   n,p=var(b,p);q=b[p:p+n];p=0;v={}
   while p<len(q):
    k,p=var(q,p)
    if k&7!=0:break
    x,p=var(q,p)
    if k>>3 in (1,2):v[str(k>>3)]=x
   d['heartbeat_fields']=v
  return d
 except (ValueError,IndexError):return {'protobuf_parse':'incomplete_or_invalid'}
def frames(key,seq,payload,d):
 st=streams.get(key)
 if not st:
  st=streams[key]={'next':seq,'buffer':b'','http':True,'pending':{}}
 if seq<st['next']:
  skip=st['next']-seq;payload=payload[skip:];seq+=skip
 if not payload:return
 if seq>st['next']:
  if len(st['pending'])<100:st['pending'][seq]=payload
  return
 st['buffer']+=payload;st['next']+=len(payload)
 while st['next'] in st['pending']:
  p=st['pending'].pop(st['next']);st['buffer']+=p;st['next']+=len(p)
 if len(st['buffer'])>262144:streams.pop(key,None);return
 if st['http']:
  i=st['buffer'].find(b'\r\n\r\n')
  if i<0:return
  st['buffer']=st['buffer'][i+4:];st['http']=False
  emit(dict(d,event='http_headers_complete'))
 while len(st['buffer'])>=2:
  b=st['buffer'];op=b[0]&15;fin=bool(b[0]&128);masked=bool(b[1]&128);n=b[1]&127;p=2
  if n==126:
   if len(b)<4:return
   n=struct.unpack('!H',b[2:4])[0];p=4
  elif n==127:
   if len(b)<10:return
   n=struct.unpack('!Q',b[2:10])[0];p=10
  if n>262144:streams.pop(key,None);return
  if masked:
   if len(b)<p+4:return
   mask=b[p:p+4];p+=4
  if len(b)<p+n:return
  q=b[p:p+n];st['buffer']=b[p+n:]
  if masked:q=bytes(x^mask[i%4] for i,x in enumerate(q))
  fields=wsmeta(q,d) if op==2 and fin else {}
  emit(dict(d,event='ws_frame',opcode=op,fin=fin,bytes=n,**fields))
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3));s.settimeout(1)
end=time.monotonic()+1200;emit({'ready':time.time(),'duration':1200})
while time.monotonic()<end:
 try:b,meta=s.recvfrom(65535)
 except socket.timeout:continue
 if len(b)<34:continue
 if b[12:14]==b'\x08\x06' and len(b)>=42:
  src=socket.inet_ntoa(b[28:32]);dst=socket.inet_ntoa(b[38:42])
  if src in IPS or dst in IPS:emit({'t':time.time(),'event':'arp','interface':meta[0],'src':src,'dst':dst,'op':struct.unpack('!H',b[20:22])[0],'smac':b[22:28].hex(':')})
  continue
 if b[12:14]!=b'\x08\x00':continue
 ip=b[14:];ih=(ip[0]&15)*4;total=struct.unpack('!H',ip[2:4])[0]
 if ih<20 or total<ih or len(ip)<total or struct.unpack('!H',ip[6:8])[0]&0x1fff:continue
 ip=ip[:total]  # exclude Ethernet padding from TCP/WebSocket reassembly
 src=socket.inet_ntoa(ip[12:16]);dst=socket.inet_ntoa(ip[16:20]);p=ip[ih:];proto=ip[9]
 if src not in IPS and dst not in IPS:continue
 d={'t':time.time(),'interface':meta[0],'src':src,'dst':dst,'proto':proto,'len':len(b),'smac':b[6:12].hex(':'),'dmac':b[:6].hex(':')}
 if proto==1 and len(p)>=8:emit(dict(d,event='icmp',type=p[0],id=struct.unpack('!H',p[4:6])[0],seq=struct.unpack('!H',p[6:8])[0]));continue
 if len(p)<8:continue
 sp,dp=struct.unpack('!HH',p[:4]);d.update(sp=sp,dp=dp)
 if proto==17 and 5353 in (sp,dp):emit(dict(d,event='dns',dns=p[8:].hex()))
 elif proto==6 and len(p)>=20 and (9443 in (sp,dp) or 7880 in (sp,dp)):
  seq,ack=struct.unpack('!II',p[4:12]);flags=p[13];th=(p[12]>>4)*4
  emit(dict(d,event='tcp',flags=flags,seq=seq,ack=ack,bytes=len(p)-th))
  key=(src,sp,dst,dp)
  if flags&2:streams.pop(key,None)
  if 7880 in (sp,dp) and len(p)>th:frames(key,seq,p[th:],d)
