"""Exercise real extension binding against a synthetic, local PG wire server.

No database service, credentials or business rows are needed. Pass CLI and
extension paths as arguments. Run inside the normal build environment.
"""
import json
import re
import socketserver
import struct
import subprocess
import sys
import threading


def packet(kind, data):
    return kind + struct.pack('!I', len(data) + 4) + data


def result(rows, columns):
    desc = struct.pack('!H', columns)
    for n in range(columns):
        desc += ('c%d' % n).encode() + b'\0' + struct.pack('!IhIhih', 0, 0, 25, -1, -1, 0)
    data = packet(b'T', desc)
    for row in rows:
        payload = struct.pack('!H', columns)
        for value in row:
            value = None if value is None else str(value).encode()
            payload += struct.pack('!i', -1 if value is None else len(value))
            if value is not None:
                payload += value
        data += packet(b'D', payload)
    return data + packet(b'C', ('SELECT %d' % len(rows)).encode() + b'\0')


class Fixture(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.queries = []
        self.errors = []
        self.fail_once = True
        self.tables = {'Foo': ['upper_id'], 'foo': ['lower_id'], 'typed': ['mood', 'pair'], 'gap': ['gap_key', 'gap_part']}
        self.tables.update(('t%04d' % i, ['col%d' % c for c in range(32)]) for i in range(550))

    def handle_error(self, request, client_address):
        self.errors.append(type(sys.exc_info()[1]).__name__)

    def query(self, sql):
        q = ' '.join(sql.split())
        self.queries.append(q)
        if q.startswith(('BEGIN', 'COMMIT', 'ROLLBACK', 'SET ')):
            return packet(b'C', b'OK\0')
        if 'SELECT version()' in q:
            return result([['PostgreSQL 15.13 on synthetic fixture', 0]], 2)
        if 'SELECT oid, nspname FROM pg_namespace' in q:
            return result([[100, 'public'], [101, 'empty']], 2)
        if 'FROM pg_enum e' in q:
            assert "n.nspname='public'" in q or "n.nspname='empty'" in q, q
            return result([[100, 900, 'mood', 'happy'], [100, 900, 'mood', 'sad']] if "'public'" in q else [], 4)
        if "pg_class.relkind = 'c'" in q:
            assert "n.nspname='public'" in q or "n.nspname='empty'" in q, q
            return result([[100, 901, 'pair', 'x', 'int4'], [100, 901, 'pair', 'label', 'text']] if "'public'" in q else [], 5)
        if q.startswith('SELECT relname FROM pg_class'):
            name = re.search(r"lower\(relname\)=lower\('([^']+)'\)", q)[1]
            matches = sorted(n for n in self.tables if n.lower() == name.lower())
            return result([[matches[0]]] if matches else [], 1)
        if 'pg_attribute' in q and 'NULL constraint_id' in q:
            assert "pg_namespace.nspname=" in q, 'database-wide metadata read'
            if "pg_namespace.nspname='empty'" in q:
                return result([], 12)
            match = re.search(r"AND relname='([^']+)'", q)
            names = [match[1]] if match else sorted(self.tables)
            if match and match[1] == 'retry' and self.fail_once:
                self.fail_once = False
                self.tables['retry'] = ['retry_id']
                return packet(b'E', b'SERROR\0CXX000\0Msynthetic one-shot failure\0\0')
            rows = []
            for name in names:
                for i, col in enumerate(self.tables.get(name, [])):
                    typ = col if name == 'typed' else 'int4'
                    rows.append([100, name, 1, col, typ, -1, 0, ([2, 4][i] if name == 'gap' else i+1), 't', None, None, None])
                if name == 'Foo':
                    rows.append([100, name, None, None, None, None, None, None, None, 902, 'p', '{1}'])
                if name == 'gap':
                    rows.append([100, name, None, None, None, None, None, None, None, 903, 'p', '{2,4}'])
            return result(rows, 12)
        if 'FROM pg_indexes' in q:
            assert 'pg_namespace.nspname=' in q, q
            return result([], 3)
        raise AssertionError(q)


class Handler(socketserver.BaseRequestHandler):
    def exact(self, size):
        data = b''
        while len(data) < size:
            part = self.request.recv(size-len(data))
            if not part:
                raise EOFError()
            data += part
        return data

    def handle(self):
        try:
            size = struct.unpack('!I', self.exact(4))[0]
            self.exact(size-4)
            self.request.sendall(packet(b'R', struct.pack('!I', 0)) +
                                 packet(b'S', b'server_version\00015.13\0') +
                                 packet(b'S', b'client_encoding\0UTF8\0') + packet(b'Z', b'I'))
            while True:
                kind = self.exact(1)
                size = struct.unpack('!I', self.exact(4))[0]
                payload = self.exact(size-4)
                if kind == b'X':
                    return
                assert kind == b'Q', kind
                response = b''
                for sql in payload.rstrip(b'\0').decode().split(';'):
                    if sql.strip():
                        response += self.server.query(sql)
                self.request.sendall(response + packet(b'Z', b'I'))
        except (EOFError, ConnectionResetError):
            pass


def main():
    fixture = Fixture()
    worker = threading.Thread(target=fixture.serve_forever, daemon=True)
    worker.start()
    base = "LOAD '%s'; ATTACH 'host=127.0.0.1 port=%d user=fixture dbname=fixture sslmode=disable' AS src (TYPE postgres, READ_ONLY);" % (sys.argv[2], fixture.server_address[1])
    sql = base + '''
DESCRIBE SELECT * FROM src.public.Foo;
DESCRIBE SELECT * FROM src.public.foo;
DESCRIBE SELECT * FROM src.public.fOo;
DESCRIBE SELECT * FROM src.public.typed;
DESCRIBE SELECT * FROM src.public.gap;
DESCRIBE SELECT * FROM src.public.retry;
DESCRIBE SELECT * FROM src.public.retry;
'''
    sql += '\n'.join('DESCRIBE SELECT * FROM src.public.t%04d;' % i for i in range(550))
    sql += '''
SELECT estimated_retained_bytes <= 16777216 AND uncached_loads > 0 AS budget_ok FROM postgres_catalog_cache_stats();
SELECT count(*) AS table_count FROM duckdb_tables() WHERE database_name='src' AND schema_name='public';
SELECT estimated_retained_bytes <= 16777216 AND estimated_live_bytes = estimated_retained_bytes AS released_ok FROM postgres_catalog_cache_stats();
SELECT count(*) FROM duckdb_indexes() WHERE database_name='src';
CALL pg_clear_cache();
SELECT estimated_live_bytes=0 AND live_entries=0 AS cleared_ok FROM postgres_catalog_cache_stats();
'''
    try:
        run = subprocess.run([sys.argv[1], '-unsigned', '-csv', '-noheader'], input=sql, capture_output=True, text=True, timeout=120)
        # The one-shot load failure is expected; the following statement must recover.
        assert run.stderr.count('synthetic one-shot failure') == 1, (run.returncode, run.stderr, run.stdout[-2000:], fixture.queries[-3:])
        assert 'does not exist' not in run.stderr, run.stderr
        assert 'retry_id,INTEGER' in run.stdout, run.stdout[-2000:]
        assert 'upper_id,INTEGER' in run.stdout and 'lower_id,INTEGER' in run.stdout
        assert 'upper_id,INTEGER,NO,PRI' in run.stdout, run.stdout[:2000]
        assert 'gap_key,INTEGER,NO,PRI' in run.stdout and 'gap_part,INTEGER,NO,PRI' in run.stdout, run.stdout[:2500]
        assert 'happy' in run.stdout and 'STRUCT' in run.stdout, run.stdout[:2000]
        assert run.stdout.rstrip().endswith('true\n555\ntrue\n0\ntrue'), run.stdout[-1000:]
        assert not fixture.errors, fixture.errors
        point = [q for q in fixture.queries if 'NULL constraint_id' in q]
        assert all("pg_namespace.nspname='public'" in q or "pg_namespace.nspname='empty'" in q for q in point)
        # No broad table query before the explicit enumeration.
        broad = [i for i, q in enumerate(point) if 'AND relname=' not in q]
        assert len(broad) == 2 and broad[0] >= 550, broad
        assert all('AND relname=' in q for q in point[:broad[0]])
        print(json.dumps({'dropped_attribute_primary_key': True, 'case_variants': True, 'enum_composite': True, 'retry': True,
                          'point_lookups': len(point)-len(broad), 'complete_enumeration_tables': 555,
                          'estimated_retained_limit_bytes': 16777216, 'clear_releases_entries': True}))
    finally:
        fixture.shutdown()
        fixture.server_close()


if __name__ == '__main__':
    main()
