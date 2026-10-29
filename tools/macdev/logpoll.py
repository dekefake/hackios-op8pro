#!/usr/bin/env python3 -I
"""logpoll.py OUT SECS: while a run is live, fetch the loader's log about every 1.5 s and append every line not seen before to OUT
(the loader's final log keeps only a window of the console; the heartbeat tails of a live run have the rest). Stops when the run returns
and then saves what the loader composed for the end of the run (loader log + the kernel's whole log, a panic's text last) as OUT.final.
Order matters: the result is looked at FIRST and the staged data fetched at once, because `oem log` composes the staging buffer afresh
from the loader's own log (32 KB) and so throws the kernel's log away."""
import subprocess, sys, time, tempfile
out, secs = sys.argv[1], float(sys.argv[2])
seen = set(); start = time.time(); end = start + secs
tmp = tempfile.mktemp()
def run(*a):
    return subprocess.run(['fastboot', *a], capture_output=True, text=True, timeout=30)
with open(out, 'a') as f:
    while time.time() < end:
        try:
            res = run('getvar', 'result'); text = res.stdout + res.stderr
            if time.time() - start > 30 and ('returned' in text or 'hung' in text):
                f.write('### run returned: ' + text.strip().split('\n')[0] + '\n')
                run('get_staged', out + '.final')
                d = open(out + '.final', 'rb').read().replace(b'\0', b'')
                open(out + '.final', 'wb').write(d)
                f.write('### final log saved: %s.final (%d bytes)\n' % (out, len(d)))
                break
            r = run('oem', 'log')
            if 'OKAY' in r.stdout + r.stderr:
                run('get_staged', tmp)
                data = open(tmp, 'rb').read().replace(b'\0', b'').decode('utf-8', 'replace')
                for l in data.split('\n'):
                    if l and l not in seen:
                        seen.add(l); f.write(l + '\n')
                f.flush()
        except Exception as e:
            f.write('### poll error %r\n' % (e,))
        time.sleep(1.0)
