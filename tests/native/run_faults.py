# Drives etest.exe against faultsrv.py and checks each scenario.
#   python run_faults.py <workdir>
import subprocess, sys, os, time, json, hashlib, random, urllib.request, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = sys.argv[1]
PORT = 18090
BASE = 'http://127.0.0.1:%d' % PORT
SIZE = 16 * 1024 * 1024
fails = 0

def get(path):
    return urllib.request.urlopen(BASE + path, timeout=10).read()

def ctl(**kw):
    get('/ctl?' + '&'.join('%s=%s' % (k, v) for k, v in kw.items()))

def stats():
    return json.loads(get('/stats'))

def expect_sha(seed):
    return hashlib.sha256(random.Random(seed).randbytes(SIZE)).hexdigest()

def parse(line):
    kv = {}
    for part in line.split(' '):
        if '=' in part and 'err' not in kv:
            k, v = part.split('=', 1); kv[k] = v
    kv['err'] = line.split('err=', 1)[1] if 'err=' in line else ''
    return kv

# opts: limit= prio= glimit= secs= from= name2= prio2= limit2= retries= (see etest.cpp)
def run(name, id_, stop=0, **opts):
    args = [os.path.join(HERE, 'etest.exe'), BASE + '/file', WORK, name, str(id_), str(stop)]
    args += ['%s=%s' % (k.rstrip('_'), v) for k, v in opts.items()]
    t0 = time.time()
    out = subprocess.run(args, capture_output=True, text=True, timeout=300).stdout.strip().splitlines()
    a = parse(out[0] if out else '')
    a['secs'] = time.time() - t0
    if len(out) > 1 and out[1].startswith('B '):
        a['B'] = parse(out[1][2:])
    return a

def near(v, want, tol=0.15):
    return abs(float(v) - want) <= tol * want

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()

def check(cond, msg):
    global fails
    print('  [%s] %s' % ('PASS' if cond else 'FAIL', msg))
    if not cond: fails += 1

def fresh():
    shutil.rmtree(WORK, ignore_errors=True); os.makedirs(WORK)
    ctl(mode='normal', seed=1, delay=0.002, lm='Mon,%2002%20Nov%202026%2010:00:00%20GMT'); get('/reset')

srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'faultsrv.py'), str(PORT)],
                       stderr=subprocess.DEVNULL)   # client aborts print tracebacks: noise
time.sleep(1.5)
try:
    good1 = expect_sha(1)
    f = lambda n: os.path.join(WORK, n)

    print('T1 normal segmented download')
    fresh(); r = run('t1.bin', 1)
    check(r['result'] == '0' and sha(f('t1.bin')) == good1, 'complete, content correct')
    check(r['resumable'] == '1' and r['mapok'] == '1', 'engine reports resumable=1 and a file map')

    print('T2 pause at ~6 MB, resume')
    fresh(); r = run('t2.bin', 2, 6 * 1024 * 1024)
    check(r['result'] == '1' and os.path.exists(f('t2.bin.ndmpart')), 'stopped, part kept')
    get('/reset'); r = run('t2.bin', 2)
    s = stats()
    check(r['result'] == '0' and sha(f('t2.bin')) == good1, 'resumed to a correct file')
    check(s['sent'] < SIZE * 0.75, 'resume fetched only the rest (%d of %d bytes)' % (s['sent'], SIZE))

    print('T3 server answers one range request with a full 200 (range broken mid-download)')
    fresh(); ctl(mode='break_once', delay=0.003); r = run('t3.bin', 3)
    s = stats()
    check(r['result'] == '0' and sha(f('t3.bin')) == good1, 'finished with correct content')
    check(s.get('broke') is True, 'the fault really happened (server sent a 200 to a range request)')
    check(r['rewound'] == '0', 'progress was never thrown away (no rewind to zero)')
    check(s['sent'] < SIZE * 2.2, 'bytes fetched %d (< 2.2x file; old code restarted from 0)' % s['sent'])

    print('T4 file changed on the server (same size, new Last-Modified) between pause and resume')
    fresh(); r = run('t4.bin', 4, 6 * 1024 * 1024)
    ctl(seed=2); r = run('t4.bin', 4)
    check(r['result'] == '0' and sha(f('t4.bin')) == expect_sha(2), 'restarted, file is the NEW version (no mix)')

    print('T5 CDN: different ETag per request, same Last-Modified -> still resumes')
    fresh(); ctl(mode='cdn_etag'); r = run('t5.bin', 5, 6 * 1024 * 1024)
    get('/reset'); r = run('t5.bin', 5)
    s = stats()
    check(r['result'] == '0' and sha(f('t5.bin')) == good1, 'complete and correct')
    check(s['sent'] < SIZE * 0.75, 'resumed instead of restarting (%d bytes)' % s['sent'])

    print('T6 server stops supporting ranges after pause (6 MB on disk)')
    fresh(); r = run('t6.bin', 6, 6 * 1024 * 1024)
    part = f('t6.bin.ndmpart'); before = os.path.getsize(part) if os.path.exists(part) else -1
    ctl(mode='noranges'); r = run('t6.bin', 6)
    check(r['result'] == '2' and 'kept' in r['err'], 'stops with an explanation: ' + r['err'][:90])
    check(os.path.exists(part) and os.path.getsize(part) == before and os.path.exists(f('t6.bin.ndmstate')),
          'downloaded part and state kept untouched')
    check(r['resumable'] == '0', 'engine reports resumable=0')
    check(r['ek'] == '2', '"server not accepting resume requests" is a fatal error (not auto-retried)')
    ctl(mode='normal'); get('/reset'); r = run('t6.bin', 6)
    s = stats()
    check(r['result'] == '0' and sha(f('t6.bin')) == good1 and s['sent'] < SIZE * 0.75,
          'when ranges come back, Resume continues from the kept part')

    print('T7 no-range server from the start: plain single download still works')
    fresh(); ctl(mode='noranges'); r = run('t7.bin', 7)
    check(r['result'] == '0' and sha(f('t7.bin')) == good1 and r['resumable'] == '0', 'complete, resumable=0')

    MiB = 1024 * 1024
    print('T8 per-download speed limit 1 MiB/s (8 connections)')
    fresh(); r = run('t8.bin', 8, limit=MiB, secs=6)
    check(r['result'] == '1' and near(r['rate'], MiB), 'measured %s B/s over 4.5 s (within 15%%)' % r['rate'])

    print('T9 global limit 2 MiB/s split by priority (High 4 : Low 1)')
    fresh(); r = run('t9a.bin', 9, glimit=2 * MiB, prio=2, name2='t9b.bin', prio2=0, secs=7, from_=2)
    a, b = int(r['rate']), int(r['B']['rate'])
    check(near(a, 1.6 * MiB) and near(b, 0.4 * MiB), 'High %d B/s, Low %d B/s (want 1.6 / 0.4 MiB/s within 15%%)' % (a, b))
    check(near(a + b, 2 * MiB), 'together %d B/s: the global limit holds' % (a + b))

    print('T9b global limit 2 MiB/s, two Normal downloads -> equal halves')
    fresh(); r = run('t9c.bin', 19, glimit=2 * MiB, name2='t9d.bin', secs=6, from_=2)
    a, b = int(r['rate']), int(r['B']['rate'])
    check(near(a, MiB) and near(b, MiB), 'Normal %d B/s, Normal %d B/s (want 1 MiB/s each)' % (a, b))

    print('T9c global limit 2 MiB/s, High capped at 0.5 MiB/s by its own limit -> Normal gets the rest')
    fresh(); r = run('t9e.bin', 29, glimit=2 * MiB, prio=2, limit=MiB // 2, name2='t9f.bin', secs=6, from_=2)
    a, b = int(r['rate']), int(r['B']['rate'])
    check(near(a, 0.5 * MiB) and near(b, 1.5 * MiB), 'High %d B/s, Normal %d B/s (want 0.5 / 1.5 MiB/s)' % (a, b))

    print('T10 no global limit: a Low download yields to a High one')
    fresh(); ctl(delay=0.2); r = run('t10a.bin', 10, prio=2, name2='t10b.bin', prio2=0, secs=4, from_=1.5)
    a, b = int(r['rate']), int(r['B']['rate'])
    check(a > 0 and 0 < b <= 0.35 * a, 'High %d B/s, Low %d B/s (Low capped to ~1/4 of High)' % (a, b))

    print('T11 one connection gets 403 while the others download -> still completes')
    fresh(); ctl(mode='forbid_once'); r = run('t11.bin', 11)
    s = stats()
    check(s['forbidden'] == 1, 'the fault really happened (one ranged request answered 403)')
    check(r['result'] == '0' and sha(f('t11.bin')) == good1, 'complete, content correct (%s)' % r['err'][:60])

    print('T12 every extra connection gets 403 -> finishes over the one connection that works')
    fresh(); ctl(mode='forbid_ranges'); r = run('t12.bin', 12)
    s = stats()
    check(s['forbidden'] >= 7 and r['result'] == '0' and sha(f('t12.bin')) == good1,
          '%d requests refused, complete and correct (%s)' % (s['forbidden'], r['err'][:60]))

    print('T12b resume where every ranged request gets 403 -> fatal error, part kept (no hang)')
    fresh(); r = run('t12b.bin', 22, 6 * 1024 * 1024)
    part = f('t12b.bin.ndmpart'); before = os.path.getsize(part) if os.path.exists(part) else -1
    ctl(mode='forbid_ranges'); r = run('t12b.bin', 22)
    check(r['result'] == '2' and '403' in r['err'] and r['ek'] == '2' and r['secs'] < 60,
          'error after %.1f s, fatal: %s' % (r['secs'], r['err'][:70]))
    check(os.path.exists(part) and os.path.getsize(part) == before, 'downloaded part kept')

    print('T13 network failure (connections dropped), 1 retry -> transient error')
    fresh(); ctl(mode='drop'); r = run('t13.bin', 13, retries=1)
    s = stats()
    check(r['result'] == '2' and r['ek'] == '1' and s['dropped'] >= 2,
          'transient (auto-retry candidate) after %d dropped requests: %s' % (s['dropped'], r['err'][:60]))

    print('Q  queue tests (refresh address, auto-retry, priority start order, live limit, actions, scheduler)')
    fresh()
    qp = subprocess.run([os.path.join(HERE, 'qtest.exe'), WORK, BASE, good1], capture_output=True, text=True, timeout=600)
    for line in qp.stdout.splitlines():
        if line.strip(): print('  ' + line)
    check(qp.returncode == 0, 'qtest exit code %d' % qp.returncode)
finally:
    srv.kill()
print('ENGINE FAULT TESTS: ' + ('all passed' if fails == 0 else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
