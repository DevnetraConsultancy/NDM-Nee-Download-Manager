# Local HTTP server with switchable faults for NDM engine tests.
#   GET /file              the test file (Range supported unless mode says otherwise)
#   GET /file2             the same file under another address (address refresh)
#   GET /short             same content, half the size (refresh: size mismatch)
#   GET /newer             same size, other Last-Modified (refresh: different file)
#   GET /ctl?mode=X&seed=N set behaviour; GET /stats -> JSON; GET /reset
# Modes: normal | break_once (next ranged request with start>0 gets a 200 full body)
#        noranges (ignore Range) | cdn_etag (new ETag per request, Last-Modified fixed)
#        forbid_once (the first ranged request with start>0 waits 0.5 s, then gets 403)
#        forbid_ranges (every ranged request with start>0 gets 403)
#        drop (file requests: connection closed without an answer = network failure)
import http.server, threading, json, random, sys, time, urllib.parse

SIZE = 16 * 1024 * 1024
state = {'mode': 'normal', 'seed': 1, 'sent': 0, 'reqs': 0, 'broke': False, 'delay': 0.002,
         'lm': 'Mon, 02 Nov 2026 10:00:00 GMT', 'forbidden': 0, 'dropped': 0}
lock = threading.Lock()
cache = {}

def content(seed):
    if seed not in cache:
        r = random.Random(seed)
        cache[seed] = r.randbytes(SIZE)
    return cache[seed]

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def reply_small(self, code, body, ctype='application/json'):
        b = body.encode()
        self.send_response(code); self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path == '/ctl':
            with lock:
                if 'mode' in q: state['mode'] = q['mode'][0]; state['broke'] = False
                if 'seed' in q: state['seed'] = int(q['seed'][0])
                if 'lm' in q: state['lm'] = q['lm'][0]
                if 'delay' in q: state['delay'] = float(q['delay'][0])
            return self.reply_small(200, '{}')
        if u.path == '/reset':
            with lock: state['sent'] = 0; state['reqs'] = 0; state['forbidden'] = 0; state['dropped'] = 0
            return self.reply_small(200, '{}')
        if u.path == '/stats':
            with lock: return self.reply_small(200, json.dumps({'sent': state['sent'], 'reqs': state['reqs'],
                                                                'broke': state['broke'], 'forbidden': state['forbidden'],
                                                                'dropped': state['dropped']}))
        if u.path not in ('/file', '/file2', '/short', '/newer'):
            return self.reply_small(404, 'no')
        with lock:
            state['reqs'] += 1
            mode, seed = state['mode'], state['seed']
            data = content(seed)
            lm = state['lm'] if seed == 1 else 'Tue, 24 Nov 2026 09:00:00 GMT'
        size = SIZE
        if u.path == '/short': size = SIZE // 2
        if u.path == '/newer': lm = 'Wed, 25 Nov 2026 08:00:00 GMT'
        if mode == 'drop':
            with lock: state['dropped'] += 1
            self.close_connection = True
            return
        rng = self.headers.get('Range')
        start, end = 0, size - 1
        partial = False
        if rng and mode != 'noranges':
            a, b = rng.replace('bytes=', '').split('-')
            start = int(a); end = min(int(b), size - 1) if b else size - 1
            partial = True
            if mode == 'break_once' and start > 0:
                with lock:
                    if not state['broke']:
                        state['broke'] = True; partial = False; start, end = 0, size - 1
            if start > 0 and mode in ('forbid_once', 'forbid_ranges'):
                forbid = False
                with lock:
                    if mode == 'forbid_ranges' or not state['broke']:
                        state['broke'] = True; state['forbidden'] += 1; forbid = True
                if forbid:
                    if mode == 'forbid_once': time.sleep(0.5)    # the other connections are receiving by now
                    return self.reply_small(403, 'forbidden', 'text/plain')
        etag = '"v%d"' % seed if mode != 'cdn_etag' else '"node%d"' % random.randint(1, 10**9)
        if u.path == '/newer': etag = '"newer"'
        self.send_response(206 if partial else 200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Accept-Ranges', 'none' if mode == 'noranges' else 'bytes')
        self.send_header('ETag', etag)
        self.send_header('Last-Modified', lm)
        self.send_header('Content-Disposition', 'attachment; filename="faulttest.bin"')
        if partial: self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.send_header('Content-Length', str(end - start + 1))
        self.end_headers()
        pos = start
        try:
            while pos <= end:
                n = min(65536, end - pos + 1)
                self.wfile.write(data[pos:pos + n]); pos += n
                with lock: state['sent'] += n
                d = state['delay']
                if d: time.sleep(d)
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            pass

srv = http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 18090), H)
srv.daemon_threads = True
srv.serve_forever()
