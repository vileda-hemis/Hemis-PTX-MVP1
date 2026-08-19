import time, glob, re, subprocess
DD="/mnt/ptx-ssd-work/w2r-fleet/datadirs"
def counts():
    ab=don=sta=0
    for p in glob.glob(DD+"/*/ptxbea/debug.log"):
        try: t=open(p,errors="replace").read()
        except OSError: continue
        ab+=t.count("ceremony ABORTED"); don+=t.count("ceremony DONE"); sta+=t.count("ceremony session STARTED")
    return ab,don,sta
def tip():
    try: return int(subprocess.run(["docker","exec","ptx-w2r-caller1","Hemis-cli","-ptxbea","-rpcuser=ptxw2rpc","-rpcpassword=ptxw2pass2026","getblockcount"],capture_output=True,text=True).stdout.strip() or 0)
    except: return 0
pa=pd=ps=None
while True:
    ab,don,sta=counts(); t=tip()
    da=("" if pa is None else " (+%d)"%(ab-pa)); dd=("" if pd is None else " (+%d)"%(don-pd)); dss=("" if ps is None else " (+%d)"%(sta-ps))
    print("tip=%d  ABORTED=%d%s  DONE=%d%s  STARTED=%d%s"%(t,ab,da,don,dd,sta,dss),flush=True)
    pa,pd,ps=ab,don,sta
    time.sleep(300)
